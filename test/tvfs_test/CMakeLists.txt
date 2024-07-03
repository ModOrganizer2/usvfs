cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(tvfs_test main.cpp)
target_link_libraries(tvfs_test
    PRIVATE test_utils usvfs_dll shared
    GTest::gtest GTest::gtest_main)
target_link_options(tvfs_test PRIVATE "/DELAYLOAD:usvfs${ARCH_POSTFIX}.dll")
set_target_properties(tvfs_test
    PROPERTIES
    FOLDER tests
    RUNTIME_OUTPUT_NAME tvfs_test${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)
