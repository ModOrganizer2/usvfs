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
    Boost::thread spdlog::spdlog_header_only
    Shlwapi comsuppw)
target_include_directories(usvfs_dll PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(usvfs_dll PRIVATE BUILDING_USVFS_DLL)
set_target_properties(usvfs_dll
    PROPERTIES
    RUNTIME_OUTPUT_NAME usvfs${ARCH_POSTFIX}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${USVFS_LIBDIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${USVFS_LIBDIR}
)

add_library(usvfs_dll ALIAS usvfs_dll)
