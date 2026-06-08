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

option(SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX
    "Use InterprocessSharedMutex for SharedMemoryManager read/write paths in writable mappings"
    OFF
)
message(STATUS "Manager shared mutex: ${SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
find_program(BASH_EXECUTABLE bash REQUIRED)

enable_testing()

add_subdirectory(interprocess)
add_subdirectory(test)

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
