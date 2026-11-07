cmake_minimum_required(VERSION 3.16)

find_package(benchmark CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS thread)

add_executable(tvfs_directory_bench main.cpp)
usvfs_set_test_properties(tvfs_directory_bench)
target_link_libraries(tvfs_directory_bench
    PRIVATE test_utils thooklib Boost::thread benchmark::benchmark)
usvfs_target_link_usvfs(tvfs_directory_bench)

# the benchmark calls the hooks directly, which are declared in a private USVFS header
get_target_property(USVFS_SOURCE_DIR usvfs_dll SOURCE_DIR)
target_include_directories(tvfs_directory_bench PRIVATE ${USVFS_SOURCE_DIR})
