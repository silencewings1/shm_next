option(SHM_NEXT_REGISTER_LARGE_STRESS_TESTS "Register manual large stress tests in CTest" OFF)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

function(shm_make_test_target_name out_var test_source)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${test_source}")
    string(REPLACE "/" "_" name "${rel}")
    string(REPLACE ".cpp" "" name "${name}")
    string(REPLACE "-" "_" name "${name}")
    set(${out_var} "${name}" PARENT_SCOPE)
endfunction()

foreach(test_source IN LISTS TEST_SOURCES)
    shm_make_test_target_name(test_name "${test_source}")
    add_executable("${test_name}" "${test_source}")
    target_link_libraries("${test_name}" PRIVATE shm_next_interprocess)
    target_include_directories("${test_name}" PRIVATE "${PROJECT_SOURCE_DIR}")
endforeach()

function(shm_add_test_labels test_name source_path)
    set(labels "")

    if(source_path MATCHES "/test/container/")
        list(APPEND labels "container")
    endif()
    if(source_path MATCHES "/test/container/node_pool/")
        list(APPEND labels "node_pool" "p0")
    endif()
    if(source_path MATCHES "/test/container/nested/")
        list(APPEND labels "nested")
    endif()
    if(source_path MATCHES "/test/container/compare/")
        list(APPEND labels "compare" "benchmark")
    endif()
    if(source_path MATCHES "benchmark[.]cpp$")
        list(APPEND labels "benchmark")
    endif()
    if(source_path MATCHES "/test/allocator/")
        list(APPEND labels "allocator")
    endif()
    if(source_path MATCHES "shm_diagnostics_stats_test[.]cpp$")
        list(APPEND labels "diagnostics" "p0")
    endif()
    if(source_path MATCHES "/test/ipc/")
        list(APPEND labels "ipc")
    endif()
    if(source_path MATCHES "shm_large_data_stress_test[.]cpp$")
        list(APPEND labels "manual" "large" "stress")
    endif()
    if(source_path MATCHES "shm_layout_version_test[.]cpp$")
        list(APPEND labels "layout" "p0")
    endif()
    if(source_path MATCHES "shm_error_system_test[.]cpp$")
        list(APPEND labels "error" "p0")
    endif()
    if(source_path MATCHES "/test/sync/")
        list(APPEND labels "sync")
    endif()
    if(source_path MATCHES "shm_shared_mutex_test[.]cpp$")
        list(APPEND labels "shared_mutex" "p0")
    endif()
    if(source_path MATCHES "shm_manager_shared_mutex_integration_test[.]cpp$")
        list(APPEND labels "shared_mutex" "manager_shared_mutex" "integration" "p0")
    endif()
    if(source_path MATCHES "shm_manager_rwlock_benchmark[.]cpp$")
        list(APPEND labels "shared_mutex" "manager_shared_mutex" "benchmark")
    endif()
    if(source_path MATCHES "shm_synchronized_test[.]cpp$")
        list(APPEND labels "synchronized" "p1")
    endif()

    if(labels)
        list(REMOVE_DUPLICATES labels)
        set_tests_properties("${test_name}" PROPERTIES LABELS "${labels}")
    endif()
endfunction()

function(shm_register_test_by_path relative_path timeout)
    set(abs_path "${PROJECT_SOURCE_DIR}/${relative_path}")
    if(EXISTS "${abs_path}")
        shm_make_test_target_name(test_name "${abs_path}")
        if(TARGET "${test_name}")
            add_test(NAME "${test_name}" COMMAND "${test_name}")
            set_tests_properties("${test_name}" PROPERTIES TIMEOUT "${timeout}")
            shm_add_test_labels("${test_name}" "${abs_path}")
        endif()
    endif()
endfunction()

function(shm_add_producer_consumer_test test_name producer_rel consumer_rel mode ready_pattern timeout)
    set(producer_abs "${PROJECT_SOURCE_DIR}/${producer_rel}")
    set(consumer_abs "${PROJECT_SOURCE_DIR}/${consumer_rel}")

    if(EXISTS "${producer_abs}" AND EXISTS "${consumer_abs}")
        shm_make_test_target_name(producer_target "${producer_abs}")
        shm_make_test_target_name(consumer_target "${consumer_abs}")
        if(TARGET "${producer_target}" AND TARGET "${consumer_target}")
            add_test(NAME "${test_name}"
                COMMAND
                    "${BASH_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/cmake/run_producer_consumer_pair.sh"
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
    endif()
endfunction()

enable_testing()

# Core tests
shm_register_test_by_path(
    "test/container/string/shm_string_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/vector/shm_vector_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/list/shm_list_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/map/shm_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/hash_map/shm_hash_map_test.cpp"
    30
)

# Benchmarks / comparisons
shm_register_test_by_path(
    "test/container/string/shm_string_benchmark.cpp"
    60
)
shm_register_test_by_path(
    "test/container/vector/shm_vector_benchmark.cpp"
    120
)
shm_register_test_by_path(
    "test/container/list/shm_list_benchmark.cpp"
    120
)
shm_register_test_by_path(
    "test/container/map/shm_map_benchmark.cpp"
    120
)
shm_register_test_by_path(
    "test/container/hash_map/shm_hash_map_benchmark.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_vector_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_vector_shared_mutex_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_vector_batch_read_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_map_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_map_batch_access_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_hash_map_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_list_perf_compare.cpp"
    120
)
shm_register_test_by_path(
    "test/container/compare/shm_benchmark.cpp"
    120
)

# Nested tests
shm_register_test_by_path(
    "test/container/nested/nested_container_matrix_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_vector_string_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_list_string_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_map_string_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_hash_map_string_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_vector_list_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_vector_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_vector_hash_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_list_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_list_hash_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/nested_map_hash_map_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/nested/shm_concurrent_process_stress_test.cpp"
    30
)

# allocator / ipc / sync
shm_register_test_by_path(
    "test/allocator/shm_offset_ptr_test.cpp"
    30
)
shm_register_test_by_path(
    "test/allocator/shm_allocator_basic_test.cpp"
    30
)
shm_register_test_by_path(
    "test/allocator/shm_allocator_fragmentation_test.cpp"
    30
)
shm_register_test_by_path(
    "test/allocator/shm_manager_lifecycle_test.cpp"
    30
)
shm_register_test_by_path(
    "test/allocator/shm_diagnostics_stats_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_open_or_create_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_layout_version_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_error_system_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_read_only_snapshot_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_crash_recovery_complex_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_shared_memory_object_test.cpp"
    30
)
shm_register_test_by_path(
    "test/ipc/shm_mapped_region_test.cpp"
    30
)
if(SHM_NEXT_REGISTER_LARGE_STRESS_TESTS)
    shm_register_test_by_path(
        "test/ipc/shm_large_data_stress_test.cpp"
        3600
    )
endif()
shm_register_test_by_path(
    "test/sync/shm_mutex_robust_test.cpp"
    30
)
shm_register_test_by_path(
    "test/sync/shm_shared_mutex_test.cpp"
    30
)
shm_register_test_by_path(
    "test/sync/shm_synchronized_test.cpp"
    30
)
shm_register_test_by_path(
    "test/sync/shm_manager_shared_mutex_integration_test.cpp"
    30
)
shm_register_test_by_path(
    "test/sync/shm_manager_rwlock_benchmark.cpp"
    120
)
shm_register_test_by_path(
    "test/sync/shm_semaphore_test.cpp"
    30
)
shm_register_test_by_path(
    "test/sync/shm_condition_test.cpp"
    30
)
shm_register_test_by_path(
    "test/container/node_pool/shm_node_pool_test.cpp"
    30
)

# producer / consumer tests
shm_add_producer_consumer_test(
    "string_producer_consumer"
    "test/container/string/shm_string_producer.cpp"
    "test/container/string/shm_string_consumer.cpp"
    "fifo"
    "Press Enter to cleanup and exit"
    60
)
shm_add_producer_consumer_test(
    "vector_producer_consumer"
    "test/container/vector/shm_vector_producer.cpp"
    "test/container/vector/shm_vector_consumer.cpp"
    "background"
    "Starting to produce sensor data"
    120
)
shm_add_producer_consumer_test(
    "list_producer_consumer"
    "test/container/list/shm_list_producer.cpp"
    "test/container/list/shm_list_consumer.cpp"
    "background"
    "Shared list ready"
    60
)
shm_add_producer_consumer_test(
    "map_producer_consumer"
    "test/container/map/shm_map_producer.cpp"
    "test/container/map/shm_map_consumer.cpp"
    "background"
    "Shared map ready"
    60
)
shm_add_producer_consumer_test(
    "hash_map_producer_consumer"
    "test/container/hash_map/shm_hash_map_producer.cpp"
    "test/container/hash_map/shm_hash_map_consumer.cpp"
    "background"
    "Shared hash_map ready"
    60
)
