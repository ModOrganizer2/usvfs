cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

# binary and binary injected
add_executable(testinject_bin testinject_bin/main.cpp)
set_target_properties(testinject_bin
    PROPERTIES
    FOLDER tests/tinjectlib
    RUNTIME_OUTPUT_NAME testinject_bin${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)

add_library(testinject_dll SHARED testinject_dll/main.cpp testinject_dll/main.h)
set_target_properties(testinject_dll
    PROPERTIES
    FOLDER tests/tinjectlib
    RUNTIME_OUTPUT_NAME testinject_dll${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)

# actual test executable
add_executable(tinjectlib_test main.cpp)
target_link_libraries(tinjectlib_test
    PRIVATE tinjectlib shared GTest::gtest GTest::gtest_main)
set_target_properties(tinjectlib_test
    PROPERTIES
    FOLDER tests/tinjectlib
    RUNTIME_OUTPUT_NAME tinjectlib_test${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)
add_dependencies(tinjectlib_test testinject_bin testinject_dll)
