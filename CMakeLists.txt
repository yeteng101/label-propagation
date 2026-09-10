cmake_minimum_required(VERSION 3.16)

project(label_propagation LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(label_propagation
    src/main.cpp
)

target_compile_features(label_propagation PRIVATE cxx_std_20)
target_link_libraries(label_propagation PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(label_propagation PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-mtune=native>
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(label_propagation PRIVATE
        $<$<CONFIG:Release>:-funroll-loops>
    )
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(IPO_SUPPORTED)
    set_property(TARGET label_propagation PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "IPO/LTO is not supported: ${IPO_ERROR}")
endif()
