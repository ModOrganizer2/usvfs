cmake_minimum_required(VERSION 3.16)

if (BUILD_USVFS_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "testing")
endif()

project(usvfs)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

set(USVFS_BINDIR ${CMAKE_CURRENT_LIST_DIR}/bin)
set(USVFS_LIBDIR ${CMAKE_CURRENT_LIST_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(ARCH_POSTFIX _x86)
else()
    set(ARCH_POSTFIX _x64)
endif()

add_subdirectory(src/shared)

add_subdirectory(src/thooklib)
add_subdirectory(src/tinjectlib)
add_subdirectory(src/usvfs_helper)

add_subdirectory(src/usvfs_dll)
add_subdirectory(src/usvfs_proxy)

if (BUILD_USVFS_TESTS)
    enable_testing()
    set(USVFS_TEST_BINDIR ${CMAKE_CURRENT_LIST_DIR}/test/bin)
    add_subdirectory(test/test_utils)
    add_subdirectory(test/tvfs_test)
endif()
