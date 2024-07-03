cmake_minimum_required(VERSION 3.16)

find_package(asmjit CONFIG REQUIRED)

file(GLOB sources "*.cpp" "*.h")
source_group("" FILES ${sources})

add_library(tinjectlib STATIC ${sources})
target_link_libraries(tinjectlib PRIVATE shared asmjit::asmjit)
target_include_directories(tinjectlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(tinjectlib PROPERTIES FOLDER injection)
