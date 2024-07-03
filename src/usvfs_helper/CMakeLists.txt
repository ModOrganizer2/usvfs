cmake_minimum_required(VERSION 3.16)

file(GLOB sources "*.cpp" "*.h")
source_group("" FILES ${sources})

add_library(usvfs_helper STATIC ${sources})
target_link_libraries(usvfs_helper PRIVATE shared tinjectlib)
set_target_properties(usvfs_helper PROPERTIES FOLDER injection)
# target_precompile_headers(shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/pch.h)
