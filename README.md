# Label Propagation

High-performance C++ implementation of synchronous label propagation for the
DolphinDB programming challenge.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release -j
```

The build produces the required executable:

```bash
./build/label_propagation input.csv
```

The program writes `output.csv` in the current working directory.

## Algorithm

Each node is initialized with its input label. In every iteration, all nodes
simultaneously choose the most frequent label among their listed neighbours.
If several labels tie, the lexicographically smallest label is selected. The
iteration repeats until no label changes.

The input `neighbours` list is authoritative and already includes the node
itself, so self is not added a second time.

## Implementation Notes

- The CSV is read through a read-only memory mapping.
- Node IDs and labels are interned to compact integer IDs without copying the
  underlying strings.
- The graph is stored in CSR form using one flat neighbour array.
- Label frequencies use per-worker timestamp arrays, avoiding hash tables and
  repeated clearing during the propagation loop.
- Nodes are updated synchronously by applying changes only after all workers
  finish an iteration.
- Nodes that cannot be affected by the previous iteration are omitted once the
  number of changes becomes small.
- Independent nodes are processed by a persistent thread pool.

## Dependencies

No third-party libraries are used. Only the C++ standard library and CMake's
standard `Threads` package are required.
