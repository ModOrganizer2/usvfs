cmake_minimum_required(VERSION 3.16)

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)

add_executable(usvfs_proxy main.cpp)
target_link_libraries(usvfs_proxy PRIVATE usvfs_dll shared usvfs_helper)
# set_target_properties(usvfs_helper PROPERTIES FOLDER injection)
# target_precompile_headers(shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/pch.h)
