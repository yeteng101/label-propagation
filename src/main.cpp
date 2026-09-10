#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

[[noreturn]] void die(const char* message) {
    std::fprintf(stderr, "label_propagation: %s\n", message);
    std::exit(EXIT_FAILURE);
}

[[noreturn]] void die_errno(const char* message) {
    std::fprintf(stderr, "label_propagation: %s: %s\n", message, std::strerror(errno));
    std::exit(EXIT_FAILURE);
}

class MappedFile {
public:
    explicit MappedFile(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) {
            die_errno("cannot open input file");
        }

        struct stat status {};
        if (::fstat(fd_, &status) != 0) {
            die_errno("cannot stat input file");
        }
        if (status.st_size <= 0) {
            die("input file is empty");
        }

        size_ = static_cast<size_t>(status.st_size);
        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            die_errno("cannot memory-map input file");
        }

        data_ = static_cast<const char*>(mapping_);
#ifdef MADV_SEQUENTIAL
        ::madvise(mapping_, size_, MADV_SEQUENTIAL);
#endif
        ::close(fd_);
        fd_ = -1;
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (mapping_ != MAP_FAILED) {
            ::munmap(mapping_, size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] const char* data() const noexcept {
        return data_;
    }

    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

private:
    int fd_ = -1;
    void* mapping_ = MAP_FAILED;
    const char* data_ = nullptr;
    size_t size_ = 0;
};

class StringTable {
public:
    explicit StringTable(size_t expected = 0) {
        size_t capacity = 16;
        const size_t target = expected + (expected >> 1) + 1;
        while (capacity < target) {
            capacity <<= 1;
        }
        table_.assign(capacity, Entry{0, kInvalidIndex});
    }

    uint32_t intern(std::string_view value) {
        const uint32_t hash = hash32(value);

        for (;;) {
            const size_t mask = table_.size() - 1;
            size_t position = hash & mask;
            while (table_[position].index != kInvalidIndex) {
                const uint32_t existing = table_[position].index;
                if (table_[position].hash == hash && strings_[existing] == value) {
                    return existing;
                }
                position = (position + 1) & mask;
            }

            if ((size_ + 1) * 10 > table_.size() * 7) {
                rehash(table_.size() << 1);
                continue;
            }

            if (strings_.size() >= kInvalidIndex) {
                die("too many distinct strings");
            }
            const uint32_t index = static_cast<uint32_t>(strings_.size());
            strings_.emplace_back(value);
            table_[position] = Entry{hash, index};
            ++size_;
            return index;
        }
    }

    [[nodiscard]] size_t size() const noexcept {
        return strings_.size();
    }

    [[nodiscard]] const std::vector<std::string_view>& strings() const noexcept {
        return strings_;
    }

    std::vector<std::string_view> release_strings() {
        table_.clear();
        table_.shrink_to_fit();
        size_ = 0;
        return std::move(strings_);
    }

private:
    struct Entry {
        uint32_t hash;
        uint32_t index;
    };

    static uint32_t hash32(std::string_view value) noexcept {
        const char* data = value.data();
        size_t length = value.size();
        uint64_t hash = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(length);

        while (length >= 8) {
            uint64_t word = 0;
            std::memcpy(&word, data, sizeof(word));
            word ^= word >> 33;
            word *= 0xff51afd7ed558ccdULL;
            word ^= word >> 33;
            hash ^= word;
            hash = (hash << 27) | (hash >> 37);
            hash = hash * 0x94d049bb133111ebULL + 0x9e3779b97f4a7c15ULL;
            data += 8;
            length -= 8;
        }

        if (length != 0) {
            uint64_t tail = 0;
            std::memcpy(&tail, data, length);
            tail ^= tail >> 33;
            tail *= 0xff51afd7ed558ccdULL;
            tail ^= tail >> 33;
            hash ^= tail;
        }

        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        return static_cast<uint32_t>(hash ^ (hash >> 32));
    }

    void rehash(size_t new_capacity) {
        std::vector<Entry> old_table;
        old_table.swap(table_);
        table_.assign(new_capacity, Entry{0, kInvalidIndex});
        const size_t mask = new_capacity - 1;

        for (const Entry& entry : old_table) {
            if (entry.index == kInvalidIndex) {
                continue;
            }
            size_t position = entry.hash & mask;
            while (table_[position].index != kInvalidIndex) {
                position = (position + 1) & mask;
            }
            table_[position] = entry;
        }
    }

    std::vector<Entry> table_;
    std::vector<std::string_view> strings_;
    size_t size_ = 0;
};

struct Field {
    const char* data = nullptr;
    size_t size = 0;
    bool quoted = false;
    bool escaped = false;
};

[[nodiscard]] std::string_view view(Field field) noexcept {
    return {field.data, field.size};
}

[[nodiscard]] Field parse_field(const char*& cursor, const char* end) {
    if (cursor < end && *cursor == '"') {
        ++cursor;
        const char* start = cursor;
        bool escaped = false;
        while (cursor < end) {
            if (*cursor != '"') {
                ++cursor;
                continue;
            }
            if (cursor + 1 < end && cursor[1] == '"') {
                escaped = true;
                cursor += 2;
                continue;
            }

            const char* close = cursor++;
            while (cursor < end && *cursor != ',') {
                ++cursor;
            }
            if (cursor < end) {
                ++cursor;
            }
            return {start, static_cast<size_t>(close - start), true, escaped};
        }
        return {start, static_cast<size_t>(cursor - start), true, escaped};
    }

    const char* start = cursor;
    while (cursor < end && *cursor != ',') {
        ++cursor;
    }
    const char* close = cursor;
    if (cursor < end) {
        ++cursor;
    }
    return {start, static_cast<size_t>(close - start), false, false};
}

[[nodiscard]] std::string_view decode_csv_field(
    Field field, std::deque<std::string>& owned_strings) {
    if (!field.escaped) {
        return view(field);
    }

    std::string decoded;
    decoded.reserve(field.size);
    const char* cursor = field.data;
    const char* const end = field.data + field.size;
    while (cursor < end) {
        if (cursor + 1 < end && cursor[0] == '"' && cursor[1] == '"') {
            decoded.push_back('"');
            cursor += 2;
        } else {
            decoded.push_back(*cursor++);
        }
    }
    owned_strings.emplace_back(std::move(decoded));
    return owned_strings.back();
}

[[nodiscard]] std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

template <typename Callback>
void for_each_neighbour(std::string_view neighbours, Callback&& callback) {
    neighbours = trim_ascii(neighbours);
    if (neighbours.size() < 2 || neighbours.front() != '[' || neighbours.back() != ']') {
        die("invalid neighbours field");
    }

    const char* cursor = neighbours.data() + 1;
    const char* end = neighbours.data() + neighbours.size() - 1;
    while (cursor < end) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
            ++cursor;
        }
        const char* start = cursor;
        while (cursor < end && *cursor != ',') {
            ++cursor;
        }
        const char* close = cursor;
        while (close > start && (close[-1] == ' ' || close[-1] == '\t')) {
            --close;
        }
        if (start != close) {
            callback(std::string_view(start, static_cast<size_t>(close - start)));
        }
        if (cursor < end) {
            ++cursor;
        }
    }
}

struct Edge {
    uint32_t from;
    uint32_t to;
};

struct Change {
    uint32_t node;
    uint32_t label;
};

struct WorkerScratch {
    std::vector<uint32_t> counts;
    std::vector<uint32_t> stamps;
    uint32_t epoch = 0;

    explicit WorkerScratch(size_t label_count)
        : counts(label_count, 0), stamps(label_count, 0) {}

    uint32_t next_epoch() {
        ++epoch;
        if (epoch == 0) {
            std::fill(stamps.begin(), stamps.end(), 0);
            epoch = 1;
        }
        return epoch;
    }
};

class ParallelExecutor {
public:
    using Job = std::function<void(size_t, size_t, size_t)>;

    ParallelExecutor(unsigned thread_count, size_t label_count)
        : thread_count_(std::max(1U, thread_count)) {
        scratches_.reserve(thread_count_);
        for (unsigned i = 0; i < thread_count_; ++i) {
            scratches_.emplace_back(label_count);
        }

        helpers_.reserve(thread_count_ - 1);
        for (unsigned i = 1; i < thread_count_; ++i) {
            helpers_.emplace_back([this, i] { helper_loop(i); });
        }
    }

    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(const ParallelExecutor&) = delete;

    ~ParallelExecutor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        start_condition_.notify_all();
        for (std::thread& helper : helpers_) {
            helper.join();
        }
    }

    void run(size_t count, const Job& job) {
        if (count == 0) {
            return;
        }
        if (thread_count_ == 1 || count < 4096) {
            job(0, 0, count);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            job_ = &job;
            count_ = count;
            next_chunk_.store(0, std::memory_order_relaxed);
            remaining_.store(helpers_.size(), std::memory_order_relaxed);
            ++generation_;
        }
        start_condition_.notify_all();

        process(0, job, count);

        std::unique_lock lock(mutex_);
        done_condition_.wait(lock, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
        job_ = nullptr;
    }

    [[nodiscard]] WorkerScratch& scratch(size_t worker_id) noexcept {
        return scratches_[worker_id];
    }

    [[nodiscard]] size_t thread_count() const noexcept {
        return thread_count_;
    }

private:
    static constexpr size_t kChunkSize = 512;

    void process(size_t worker_id, const Job& job, size_t count) {
        for (;;) {
            const size_t begin = next_chunk_.fetch_add(kChunkSize, std::memory_order_relaxed);
            if (begin >= count) {
                return;
            }
            const size_t end = std::min(begin + kChunkSize, count);
            job(worker_id, begin, end);
        }
    }

    void helper_loop(size_t worker_id) {
        uint64_t seen_generation = 0;
        for (;;) {
            const Job* job = nullptr;
            size_t count = 0;
            {
                std::unique_lock lock(mutex_);
                start_condition_.wait(lock, [this, seen_generation] {
                    return stopping_ || generation_ != seen_generation;
                });
                if (stopping_) {
                    return;
                }
                seen_generation = generation_;
                job = job_;
                count = count_;
            }

            process(worker_id, *job, count);
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                done_condition_.notify_one();
            }
        }
    }

    unsigned thread_count_;
    std::vector<WorkerScratch> scratches_;
    std::vector<std::thread> helpers_;

    std::mutex mutex_;
    std::condition_variable start_condition_;
    std::condition_variable done_condition_;
    bool stopping_ = false;
    uint64_t generation_ = 0;
    const Job* job_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_chunk_{0};
    std::atomic<size_t> remaining_{0};
};

class BufferedOutput {
public:
    explicit BufferedOutput(const char* path) {
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd_ < 0) {
            die_errno("cannot create output.csv");
        }
        buffer_.resize(1U << 20);
    }

    BufferedOutput(const BufferedOutput&) = delete;
    BufferedOutput& operator=(const BufferedOutput&) = delete;

    ~BufferedOutput() {
        if (fd_ >= 0) {
            if (!flush()) {
                std::fprintf(stderr, "label_propagation: failed to write output.csv: %s\n",
                             std::strerror(errno));
            }
            ::close(fd_);
        }
    }

    void append(std::string_view value) {
        if (value.size() > buffer_.size() - position_) {
            flush_checked();
            if (value.size() >= buffer_.size()) {
                write_all(value.data(), value.size());
                return;
            }
        }
        std::memcpy(buffer_.data() + position_, value.data(), value.size());
        position_ += value.size();
    }

    void put(char value) {
        if (position_ == buffer_.size()) {
            flush_checked();
        }
        buffer_[position_++] = value;
    }

    void append_csv_field(std::string_view value) {
        bool needs_quotes = false;
        for (const char character : value) {
            if (character == ',' || character == '"' || character == '\n' ||
                character == '\r') {
                needs_quotes = true;
                break;
            }
        }
        if (!needs_quotes) {
            append(value);
            return;
        }

        put('"');
        size_t start = 0;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '"') {
                continue;
            }
            append(value.substr(start, i - start));
            append("\"\"");
            start = i + 1;
        }
        append(value.substr(start));
        put('"');
    }

    void finish() {
        flush_checked();
    }

private:
    void write_all(const char* data, size_t size) {
        while (size != 0) {
            const ssize_t written = ::write(fd_, data, size);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                die_errno("failed to write output.csv");
            }
            data += written;
            size -= static_cast<size_t>(written);
        }
    }

    bool flush() noexcept {
        if (position_ == 0) {
            return true;
        }
        const char* data = buffer_.data();
        size_t size = position_;
        while (size != 0) {
            const ssize_t written = ::write(fd_, data, size);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            data += written;
            size -= static_cast<size_t>(written);
        }
        position_ = 0;
        return true;
    }

    void flush_checked() {
        if (!flush()) {
            die_errno("failed to write output.csv");
        }
    }

    int fd_ = -1;
    std::vector<char> buffer_;
    size_t position_ = 0;
};

struct Graph {
    std::vector<std::string_view> node_ids;
    std::vector<std::string_view> label_texts;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> neighbours;
    std::vector<uint32_t> labels;
    std::unique_ptr<std::deque<std::string>> owned_strings;
};

Graph read_graph(const MappedFile& input) {
    const char* cursor = input.data();
    const char* const end = input.data() + input.size();

    const size_t reserve_hint = std::min(input.size() / 16 + 16, size_t{16} << 20);
    StringTable node_table(reserve_hint);
    StringTable label_table(1024);

    std::vector<uint32_t> node_labels;
    std::vector<uint32_t> degrees;
    std::vector<Edge> edges;
    auto owned_strings = std::make_unique<std::deque<std::string>>();
    edges.reserve(std::min(input.size() / 8 + 16, size_t{200} << 20));

    auto get_node = [&](std::string_view id) -> uint32_t {
        const uint32_t index = node_table.intern(id);
        if (index >= node_labels.size()) {
            node_labels.push_back(kInvalidIndex);
            degrees.push_back(0);
        }
        return index;
    };

    bool saw_header = false;
    while (cursor < end) {
        const char* line_start = cursor;
        const void* newline = std::memchr(cursor, '\n', static_cast<size_t>(end - cursor));
        const char* line_end =
            newline == nullptr ? end : static_cast<const char*>(newline);
        cursor = newline == nullptr ? end : line_end + 1;

        if (line_end > line_start && line_end[-1] == '\r') {
            --line_end;
        }
        if (line_start == line_end) {
            continue;
        }
        if (!saw_header) {
            saw_header = true;
            continue;
        }

        const char* field_cursor = line_start;
        const Field id_field = parse_field(field_cursor, line_end);
        const Field label_field = parse_field(field_cursor, line_end);
        const Field neighbours_field = parse_field(field_cursor, line_end);

        const std::string_view id = decode_csv_field(id_field, *owned_strings);
        const std::string_view label = decode_csv_field(label_field, *owned_strings);
        const std::string_view neighbours =
            decode_csv_field(neighbours_field, *owned_strings);

        const uint32_t node = get_node(id);
        if (node_labels[node] != kInvalidIndex) {
            die("duplicate node_id in input");
        }
        node_labels[node] = label_table.intern(label);

        uint32_t degree = 0;
        for_each_neighbour(neighbours, [&](std::string_view neighbour_id) {
            const uint32_t neighbour = get_node(neighbour_id);
            edges.push_back(Edge{node, neighbour});
            ++degree;
        });
        degrees[node] = degree;
    }

    if (!saw_header) {
        die("input has no header row");
    }

    const size_t node_count = node_labels.size();
    for (uint32_t label : node_labels) {
        if (label == kInvalidIndex) {
            die("a neighbour ID has no corresponding node row");
        }
    }

    std::vector<uint64_t> offsets(node_count + 1, 0);
    for (size_t i = 0; i < node_count; ++i) {
        offsets[i + 1] = offsets[i] + degrees[i];
    }
    if (offsets.back() > std::numeric_limits<size_t>::max()) {
        die("graph is too large");
    }

    std::fill(degrees.begin(), degrees.end(), 0);
    std::vector<uint32_t> neighbours(static_cast<size_t>(offsets.back()));
    for (const Edge edge : edges) {
        neighbours[static_cast<size_t>(offsets[edge.from] + degrees[edge.from]++)] =
            edge.to;
    }
    edges.clear();
    edges.shrink_to_fit();
    degrees.clear();
    degrees.shrink_to_fit();

    const size_t label_count = label_table.size();
    std::vector<uint32_t> order(label_count);
    for (size_t i = 0; i < label_count; ++i) {
        order[i] = static_cast<uint32_t>(i);
    }
    const auto& raw_label_texts = label_table.strings();
    std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
        return raw_label_texts[left] < raw_label_texts[right];
    });

    std::vector<uint32_t> rank(label_count);
    std::vector<std::string_view> sorted_label_texts(label_count);
    for (size_t i = 0; i < label_count; ++i) {
        rank[order[i]] = static_cast<uint32_t>(i);
        sorted_label_texts[i] = raw_label_texts[order[i]];
    }
    for (uint32_t& label : node_labels) {
        label = rank[label];
    }

    Graph graph;
    graph.node_ids = node_table.release_strings();
    graph.label_texts = std::move(sorted_label_texts);
    graph.offsets = std::move(offsets);
    graph.neighbours = std::move(neighbours);
    graph.labels = std::move(node_labels);
    graph.owned_strings = std::move(owned_strings);
    return graph;
}

unsigned choose_thread_count(size_t node_count, size_t label_count) {
    unsigned hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        hardware_threads = 1;
    }
    if (node_count < 100'000 || label_count == 0) {
        return 1;
    }

    constexpr size_t kScratchBudget = size_t{3} << 30;
    const size_t scratch_per_thread = label_count * 2 * sizeof(uint32_t);
    unsigned memory_limited = hardware_threads;
    if (scratch_per_thread != 0) {
        memory_limited = static_cast<unsigned>(
            std::max<size_t>(1, kScratchBudget / scratch_per_thread));
    }
    return std::max(1U, std::min(hardware_threads, memory_limited));
}

void run_label_propagation(Graph& graph) {
    const size_t node_count = graph.node_ids.size();
    if (node_count == 0) {
        return;
    }

    const unsigned thread_count =
        choose_thread_count(node_count, graph.label_texts.size());
    ParallelExecutor executor(thread_count, graph.label_texts.size());
    std::vector<std::vector<Change>> changes(executor.thread_count());

    std::vector<uint32_t> active;
    std::vector<uint32_t> next_active;
    std::vector<uint32_t> active_stamp(node_count, 0);
    uint32_t active_epoch = 0;
    bool full_sweep = true;

    for (;;) {
        const size_t work_count = full_sweep ? node_count : active.size();
        for (std::vector<Change>& worker_changes : changes) {
            worker_changes.clear();
        }

        const auto job = [&](size_t worker_id, size_t begin, size_t end) {
            WorkerScratch& scratch = executor.scratch(worker_id);
            std::vector<Change>& worker_changes = changes[worker_id];
            const std::vector<uint32_t>& labels = graph.labels;
            const std::vector<uint32_t>& neighbours = graph.neighbours;
            const std::vector<uint64_t>& offsets = graph.offsets;

            for (size_t position = begin; position < end; ++position) {
                const uint32_t node =
                    full_sweep ? static_cast<uint32_t>(position) : active[position];
                const uint32_t epoch = scratch.next_epoch();
                const uint64_t edge_begin = offsets[node];
                const uint64_t edge_end = offsets[node + 1];

                uint32_t best_label = kInvalidIndex;
                uint32_t best_count = 0;
                for (uint64_t edge = edge_begin; edge < edge_end; ++edge) {
                    const uint32_t label = labels[neighbours[static_cast<size_t>(edge)]];
                    if (scratch.stamps[label] != epoch) {
                        scratch.stamps[label] = epoch;
                        scratch.counts[label] = 1;
                        if (best_count == 0 || 1 > best_count ||
                            (1 == best_count && label < best_label)) {
                            best_count = 1;
                            best_label = label;
                        }
                    } else {
                        const uint32_t count = ++scratch.counts[label];
                        if (count > best_count ||
                            (count == best_count && label < best_label)) {
                            best_count = count;
                            best_label = label;
                        }
                    }
                }

                if (best_count == 0) {
                    best_label = labels[node];
                }
                if (best_label != labels[node]) {
                    worker_changes.push_back(Change{node, best_label});
                }
            }
        };

        executor.run(work_count, job);

        size_t change_count = 0;
        for (const std::vector<Change>& worker_changes : changes) {
            change_count += worker_changes.size();
        }
        if (change_count == 0) {
            break;
        }

        std::vector<uint32_t> changed_nodes;
        changed_nodes.reserve(change_count);
        for (const std::vector<Change>& worker_changes : changes) {
            for (const Change change : worker_changes) {
                graph.labels[change.node] = change.label;
                changed_nodes.push_back(change.node);
            }
        }

        if (full_sweep) {
            if (change_count * 16 >= node_count) {
                continue;
            }
        }

        ++active_epoch;
        if (active_epoch == 0) {
            std::fill(active_stamp.begin(), active_stamp.end(), 0);
            active_epoch = 1;
        }

        next_active.clear();
        for (const uint32_t node : changed_nodes) {
            if (active_stamp[node] != active_epoch) {
                active_stamp[node] = active_epoch;
                next_active.push_back(node);
            }
            for (uint64_t edge = graph.offsets[node]; edge < graph.offsets[node + 1];
                 ++edge) {
                const uint32_t neighbour = graph.neighbours[static_cast<size_t>(edge)];
                if (active_stamp[neighbour] != active_epoch) {
                    active_stamp[neighbour] = active_epoch;
                    next_active.push_back(neighbour);
                }
            }
        }

        if (next_active.size() * 2 >= node_count) {
            full_sweep = true;
            active.clear();
        } else {
            full_sweep = false;
            active.swap(next_active);
        }
    }
}

void write_output(const Graph& graph) {
    const size_t node_count = graph.node_ids.size();
    std::vector<uint32_t> order(node_count);
    for (size_t i = 0; i < node_count; ++i) {
        order[i] = static_cast<uint32_t>(i);
    }

    const auto less = [&](uint32_t left, uint32_t right) {
        return graph.node_ids[left] < graph.node_ids[right];
    };
    if (!std::is_sorted(order.begin(), order.end(), less)) {
        std::sort(order.begin(), order.end(), less);
    }

    BufferedOutput output("output.csv");
    output.append("node_id,final_label\n");
    for (const uint32_t node : order) {
        output.append_csv_field(graph.node_ids[node]);
        output.put(',');
        output.append_csv_field(graph.label_texts[graph.labels[node]]);
        output.put('\n');
    }
    output.finish();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <input_file.csv>\n", argv[0]);
        return EXIT_FAILURE;
    }

    MappedFile input(argv[1]);
    Graph graph = read_graph(input);
    run_label_propagation(graph);
    write_output(graph);
    return EXIT_SUCCESS;
}
