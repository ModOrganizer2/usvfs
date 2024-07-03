cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS thread)

# not sure why there is a test_hooks.cpp here?
add_executable(thooklib_test main.cpp)
target_link_libraries(thooklib_test
    PRIVATE test_utils shared thooklib Boost::thread
    GTest::gtest GTest::gtest_main)
set_target_properties(thooklib_test
    PROPERTIES
    FOLDER tests
    RUNTIME_OUTPUT_NAME thooklib_test${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)
