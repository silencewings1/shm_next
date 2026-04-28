cmake_minimum_required(VERSION 3.20)

project(shm_next VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

foreach(test_name IN ITEMS
    shm_semaphore
    shm_mutex_robust
    shm_open_or_create
    shm_manager_lifecycle
)
    if(TARGET "${test_name}")
        add_test(NAME "${test_name}" COMMAND "${test_name}")
    endif()
endforeach()
