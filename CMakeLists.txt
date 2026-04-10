cmake_minimum_required(VERSION 3.20)

project(shm_next LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Threads REQUIRED)

file(GLOB_RECURSE INTERPROCESS_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/interprocess/*.h"
)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp"
)

add_library(shm_next_interprocess STATIC
    interprocess/interprocess.cpp
)

target_sources(shm_next_interprocess
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${INTERPROCESS_HEADERS}
)

target_include_directories(shm_next_interprocess
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(shm_next_interprocess
    PUBLIC
        Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(shm_next_interprocess PUBLIC rt)
endif()

foreach(test_source IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)

    add_executable("${test_name}" "${test_source}")
    target_link_libraries("${test_name}" PRIVATE shm_next_interprocess)
    target_include_directories("${test_name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
