cmake_minimum_required(VERSION 3.16)

find_package(asmjit CONFIG REQUIRED)

file(GLOB sources "*.cpp" "*.h")
source_group("" FILES ${sources})

add_library(thooklib STATIC ${sources})
target_link_libraries(thooklib PRIVATE shared asmjit::asmjit)
set_target_properties(thooklib PROPERTIES FOLDER injection)
target_precompile_headers(shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/pch.h)
