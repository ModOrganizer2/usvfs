cmake_minimum_required(VERSION 3.16)

file(GLOB directories LIST_DIRECTORIES true "*")

# this goes through all the directories and
#
# 1. add them if there is a CMakeLists.txt inside
# 2. add correspondings tests (for CTest) for BOTH x86 and x64 when possible
#
foreach(directory ${directories})
    if(NOT(IS_DIRECTORY ${directory}))
        continue()
    endif()

    if(NOT(EXISTS ${directory}/CMakeLists.txt))
        continue()
    endif()

    add_subdirectory(${directory})

    get_filename_component(dirname ${directory} NAME)
    if(dirname STREQUAL "test_utils")
        continue()
    endif()

    add_test(NAME ${dirname}_x64
        COMMAND ${USVFS_TEST_BINDIR}/${dirname}_x64.exe
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    add_test(NAME ${dirname}_x86
        COMMAND ${USVFS_TEST_BINDIR}/${dirname}_x86.exe
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
endforeach()
