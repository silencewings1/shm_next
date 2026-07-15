add_library(shm_next_interprocess STATIC
    interprocess.cpp
)

add_library(shm_next::interprocess ALIAS shm_next_interprocess)

set_target_properties(shm_next_interprocess PROPERTIES
    EXPORT_NAME interprocess
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(shm_next_interprocess
    PUBLIC
        cxx_std_17
)

target_include_directories(shm_next_interprocess
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(shm_next_interprocess
    PUBLIC
        Threads::Threads
)

if(SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX)
    target_compile_definitions(shm_next_interprocess
        PUBLIC SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX=1
    )
else()
    target_compile_definitions(shm_next_interprocess
        PUBLIC SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX=0
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(shm_next_interprocess PUBLIC rt)
endif()

install(TARGETS shm_next_interprocess
    EXPORT shm_nextTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/interprocess"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h"
)
