cmake_minimum_required(VERSION 3.20)

project(shm_next VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the build type"
        FORCE
    )
else()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" SHM_NEXT_BUILD_TYPE_UPPER)
    if(SHM_NEXT_BUILD_TYPE_UPPER STREQUAL "DEBUG")
        set(CMAKE_BUILD_TYPE Debug CACHE STRING
            "Choose the build type"
            FORCE
        )
    elseif(SHM_NEXT_BUILD_TYPE_UPPER STREQUAL "RELEASE")
        set(CMAKE_BUILD_TYPE Release CACHE STRING
            "Choose the build type"
            FORCE
        )
    else()
        message(FATAL_ERROR
            "Unsupported CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. Use Debug or Release."
        )
    endif()
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
find_program(BASH_EXECUTABLE bash REQUIRED)

file(GLOB_RECURSE INTERPROCESS_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/interprocess/*.h"
)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp"
)

add_library(shm_next_interprocess STATIC
    interprocess/interprocess.cpp
)

add_library(shm_next::interprocess ALIAS shm_next_interprocess)

set_target_properties(shm_next_interprocess PROPERTIES
    EXPORT_NAME interprocess
)

target_compile_features(shm_next_interprocess
    PUBLIC
        cxx_std_17
)

target_sources(shm_next_interprocess
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${INTERPROCESS_HEADERS}
)

target_include_directories(shm_next_interprocess
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(shm_next_interprocess
    PUBLIC
        Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(shm_next_interprocess PUBLIC rt)
endif()

install(TARGETS shm_next_interprocess
    EXPORT shm_nextTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(EXPORT shm_nextTargets
    NAMESPACE shm_next::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/shm_next"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shm_nextConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shm_nextConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/shm_next"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/shm_nextConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/shm_nextConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/shm_nextConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/shm_next"
)

foreach(test_source IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)

    add_executable("${test_name}" "${test_source}")
    target_link_libraries("${test_name}" PRIVATE shm_next_interprocess)
    target_include_directories("${test_name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

enable_testing()

set(SHM_NEXT_CTEST_TARGETS
    shm_semaphore
    shm_mutex_robust
    shm_open_or_create
    shm_manager_lifecycle
    shm_container_stl_semantics
    shm_map_perf_compare
    shm_vector_perf_compare
    shm_allocator_fragmentation
    shm_concurrent_process_stress
    shm_crash_recovery_complex
    shm_read_only_snapshot
)

foreach(test_name IN LISTS SHM_NEXT_CTEST_TARGETS)
    if(TARGET "${test_name}")
        add_test(NAME "${test_name}" COMMAND "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 30)
    endif()
endforeach()

function(add_producer_consumer_test test_name producer_target consumer_target mode ready_pattern timeout)
    if(TARGET "${producer_target}" AND TARGET "${consumer_target}")
        add_test(NAME "${test_name}"
            COMMAND
                "${BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_producer_consumer_pair.sh"
                --producer "$<TARGET_FILE:${producer_target}>"
                --consumer "$<TARGET_FILE:${consumer_target}>"
                --mode "${mode}"
                --ready-pattern "${ready_pattern}"
                --ready-timeout 30
        )
        set_tests_properties("${test_name}" PROPERTIES
            TIMEOUT "${timeout}"
            LABELS "integration;producer;consumer"
        )
    endif()
endfunction()

add_producer_consumer_test(
    shm_string_producer_consumer
    shm_string_producer
    shm_string_consumer
    fifo
    "Press Enter to cleanup and exit"
    60
)

add_producer_consumer_test(
    shm_nested_producer_consumer
    shm_nested_producer
    shm_nested_consumer
    fifo
    "Data written. Press Enter"
    60
)

add_producer_consumer_test(
    shm_map_producer_consumer
    shm_map_producer
    shm_map_consumer
    background
    "Shared map ready"
    60
)

add_producer_consumer_test(
    shm_vector_producer_consumer
    shm_vector_producer
    shm_vector_consumer
    background
    "Starting to produce sensor data"
    120
)

if(TEST shm_allocator_fragmentation)
    set_tests_properties(shm_allocator_fragmentation PROPERTIES
        LABELS "complex;allocator"
    )
endif()

if(TEST shm_concurrent_process_stress)
    set_tests_properties(shm_concurrent_process_stress PROPERTIES
        LABELS "complex;process;container"
    )
endif()

if(TEST shm_crash_recovery_complex)
    set_tests_properties(shm_crash_recovery_complex PROPERTIES
        LABELS "complex;process;recovery"
    )
endif()

if(TEST shm_read_only_snapshot)
    set_tests_properties(shm_read_only_snapshot PROPERTIES
        LABELS "complex;readonly"
    )
endif()
