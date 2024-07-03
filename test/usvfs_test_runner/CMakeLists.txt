cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

# other executables
add_executable(test_file_operations
    test_file_operations/test_file_operations.cpp
    test_file_operations/test_filesystem.cpp
    test_file_operations/test_filesystem.h
    test_file_operations/test_ntapi.cpp
    test_file_operations/test_ntapi.h
    test_file_operations/test_ntdll_declarations.h
    test_file_operations/test_w32api.cpp
    test_file_operations/test_w32api.h
)
target_link_libraries(test_file_operations PRIVATE test_utils ntdll)
set_target_properties(test_file_operations
    PROPERTIES
    FOLDER tests/usvfs_test_runner
    RUNTIME_OUTPUT_NAME test_file_operations${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)

add_executable(usvfs_test
    usvfs_test/usvfs_basic_test.cpp
    usvfs_test/usvfs_basic_test.h
    usvfs_test/usvfs_test_base.cpp
    usvfs_test/usvfs_test_base.h
    usvfs_test/usvfs_test.cpp
)
target_link_libraries(usvfs_test PRIVATE test_utils usvfs_dll)
target_link_options(usvfs_test PRIVATE "/DELAYLOAD:usvfs${ARCH_POSTFIX}.dll")
set_target_properties(usvfs_test
    PROPERTIES
    FOLDER tests/usvfs_test_runner
    RUNTIME_OUTPUT_NAME usvfs_test${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)

# actual test executable
add_executable(usvfs_test_runner usvfs_test_runner.cpp)
target_link_libraries(usvfs_test_runner
    PRIVATE test_utils GTest::gtest GTest::gtest_main)
set_target_properties(usvfs_test_runner
    PROPERTIES
    FOLDER tests/usvfs_test_runner
    RUNTIME_OUTPUT_NAME usvfs_test_runner${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_TEST_BINDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_TEST_BINDIR}
)
add_dependencies(usvfs_test_runner usvfs_test test_file_operations)
