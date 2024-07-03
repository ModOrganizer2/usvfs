cmake_minimum_required(VERSION 3.16)

project(usvfs)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(src/shared)

add_subdirectory(src/thooklib)
add_subdirectory(src/tinjectlib)
add_subdirectory(src/usvfs_helper)
