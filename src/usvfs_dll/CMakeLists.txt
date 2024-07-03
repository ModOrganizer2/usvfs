cmake_minimum_required(VERSION 3.16)

find_package(Boost CONFIG REQUIRED COMPONENTS thread)
find_package(spdlog CONFIG REQUIRED)

file(GLOB sources "*.cpp" "*.h")
source_group("" FILES ${sources})

file(GLOB hooks "hooks/*.cpp" "hooks/*.h")
source_group("dlls" FILES ${hooks})

add_library(usvfs_dll SHARED ${sources} ${hooks})
target_link_libraries(usvfs_dll
    PUBLIC shared thooklib usvfs_helper
    Boost::thread spdlog::spdlog)
target_compile_definitions(usvfs_dll PRIVATE BUILDING_USVFS_DLL)
# target_precompile_headers(shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pch.h)

add_library(usvfs ALIAS usvfs_dll)
