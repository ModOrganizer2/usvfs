cmake_minimum_required(VERSION 3.16)

find_package(asmjit CONFIG REQUIRED)
find_library(LIBUDIS86_LIBRARY libudis86)

file(GLOB sources CONFIGURE_DEPENDS "*.cpp" "*.h")
source_group("" FILES ${sources})

add_library(thooklib STATIC ${sources})
message(STATUS "VCPKG_ROOT=${VCPKG_ROOT}")
target_link_libraries(thooklib PRIVATE shared asmjit::asmjit ${LIBUDIS86_LIBRARY})
target_include_directories(thooklib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(thooklib PROPERTIES FOLDER injection)
target_precompile_headers(shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/pch.h)
