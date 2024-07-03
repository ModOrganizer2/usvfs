cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(shared_test main.cpp)
target_link_libraries(shared_test
    PRIVATE test_utils shared
    GTest::gtest GTest::gtest_main)
set_target_properties(shared_test
    PROPERTIES
    FOLDER tests
    RUNTIME_OUTPUT_NAME shared_test${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)
