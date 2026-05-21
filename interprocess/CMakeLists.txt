file(GLOB_RECURSE INTERPROCESS_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(shm_next_interprocess STATIC
    interprocess.cpp
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
        BASE_DIRS "${PROJECT_SOURCE_DIR}"
        FILES ${INTERPROCESS_HEADERS}
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
