cmake_minimum_required(VERSION 3.14)
project(taper-microbench CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=armv8-a+crc -flto=thin")
endif()

include(FetchContent)
FetchContent_Declare(xxhash GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git GIT_TAG v0.8.2)
FetchContent_MakeAvailable(xxhash)

# ═══════════════════════════════════════════════════════════════════
# Micro benchmark targets
# ═══════════════════════════════════════════════════════════════════

# 1. Pure hashmap insert+probe (no serialize/arena)
add_executable(cpp_micro_hashmap src/micro_hashmap_bench.cpp)
target_include_directories(cpp_micro_hashmap PRIVATE include ${xxhash_SOURCE_DIR})

# 2. Individual functions (arena, newrow, serialize, compare, hashmap, setptr)
add_executable(cpp_micro_functions src/micro_functions_bench.cpp)
target_include_directories(cpp_micro_functions PRIVATE include ${xxhash_SOURCE_DIR})

# 3. Isolated cases (serialize_only, compare_only, hashmap+empty_row)
add_executable(cpp_micro_cases src/micro_cases_bench.cpp)
target_include_directories(cpp_micro_cases PRIVATE include ${xxhash_SOURCE_DIR})

# 4. Pipeline layers (A: hashmap, C: +newrow, C2: +serialize, D: full)
add_executable(cpp_micro_pipeline src/micro_pipeline_bench.cpp)
target_include_directories(cpp_micro_pipeline PRIVATE include ${xxhash_SOURCE_DIR})

# 5. Standalone profile runner (full 4str_0int workload, no framework)
add_executable(cpp_profile_taper src/profile_taper_standalone.cpp)
target_include_directories(cpp_profile_taper PRIVATE include ${xxhash_SOURCE_DIR})

# 6. Step-by-step: each function in the pipeline
add_executable(cpp_step_by_step src/step_by_step_bench.cpp)
target_include_directories(cpp_step_by_step PRIVATE include ${xxhash_SOURCE_DIR})

# 7. EmplaceBatch breakdown: every sub-function individually
add_executable(cpp_emplace_breakdown src/emplace_breakdown_bench.cpp)
target_include_directories(cpp_emplace_breakdown PRIVATE include ${xxhash_SOURCE_DIR})
