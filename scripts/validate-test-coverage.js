'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const TEST_DIR = path.join(ROOT_DIR, 'test');
const MAIN_FILE = path.join(TEST_DIR, 'test_main.cpp');
const PACKAGE_FILE = path.join(ROOT_DIR, 'package.json');
const CI_FILE = path.join(ROOT_DIR, '.github', 'workflows', 'ci.yml');
const PLATFORMIO_FILE = path.join(ROOT_DIR, 'platformio.ini');
const UNIT_RUNNER_FILE = path.join(ROOT_DIR, 'scripts', 'run-unit-tests.js');
const TEST_ALL_FILE = path.join(ROOT_DIR, 'scripts', 'test-all.ps1');
const EXCLUDED_TEST_DIRS = new Set(['browser', 'e2e', 'fixtures', 'mocks', 'test_embedded', 'unit']);
const CRITICAL_REGRESSION_CONTRACTS = [
    {
        name: 'panel navigation cancellation excludes genuine device request failures',
        files: {
            'test/unit/test_device_menu_read_only.py': [
                'class DeviceMenuReadOnlyTests',
                'def test_panel_navigation_abort_contract_rejects_real_failures',
                'def test_status_post_allowlist_rejects_movement_and_ambiguous_payloads',
                'def test_browser_scripts_block_unknown_writes_and_fail_on_diagnostics'
            ],
            'scripts/test-screen-panel-browser-device.mjs': [
                'function isIntentionalBatchNavigationAbort(',
                "failure !== 'net::ERR_ABORTED'",
                'generation >= navigation.generation',
                'until: at + 1500',
                "assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 1), false)",
                "assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 0, 2501), false)"
            ]
        }
    },
    {
        name: 'web menu traversal exempts only navigation-window read-only aborts',
        files: {
            'test/unit/test_menu_cancellation_evidence.py': [
                'def test_navigation_aborted_reads_exemption_keeps_real_failures'
            ],
            'scripts/lib/menu-cancellation-evidence.mjs': [
                'export function isNavigatedAbortedRead(',
                'GOVERNOR_TIMEOUT_ABORT_EXCLUSION_MS'
            ],
            'scripts/test-web-device-menus.mjs': [
                'isNavigatedAbortedRead(',
                'report.navigatedAborted.push(',
                'navigation = { at:'
            ]
        }
    },
    {
        name: 'Basic finite responses bound real write failures without empty-send or duplicate-ACK progress',
        files: {
            'test/unit/test_web_basic_progress.py': [
                'class WebBasicProgressTests',
                'def test_original_add_failure_empty_send_stalls_at_30s_and_wrap',
                'def test_actual_basic_deadlines_recovery_duplicate_ack_and_ownership',
                'def test_original_basic_fails_the_bounded_regression'
            ],
            'test/unit/cpp/web_basic_progress_harness.cpp': [
                '#define FASTBEE_FINITE_REFERENCE_MAIN',
                'return clock.add(data,length);',
                'initialWriteFailure(0);initialWriteFailure(UINT32_MAX-5000U);',
                'partialThenStall();recoveryBeforeDeadline();pendingFlushIsNotProgress();',
                'zeroSpaceAndDuplicateCallbacks();slowRealProgressAndOwnership();',
                'f.clock._rx_last_ack=f.clock._rx_last_packet=at;',
                'assert(f.closes==1&&f.disconnects==1);'
            ]
        }
    },
{
    "name": "TCP admission diagnostics report the existing atomic count and reject partial dependency patches",
    "files": {
        "test/unit/test_asynctcp_admission_snapshot.py": [
            "class AsyncTcpAdmissionSnapshotTests",
            "def test_existing_source_is_preserved_and_patch_is_idempotent",
            "def test_partial_duplicate_and_changed_providers_are_rejected",
            "def test_real_provider_reads_current_count_caps_and_rejects_null_arguments",
            "def test_missing_weak_provider_reports_unavailable"
        ]
    }
},
{
    "name": "configuration document saves compare complete readback without a duplicate DOM and preserve generations on faults",
    "files": {
        "test/unit/test_config_storage_save_peak.py": [
            "class ConfigStorageSavePeakTests",
            "def test_complete_readback_faults_atomic_commit_and_measured_peak",
            "def test_both_document_saves_are_pinned_to_the_borrowed_source"
        ],
        "test/unit/cpp/config_save_peak_harness.cpp.in": [
            "assert(verifyJsonFile(\"candidate\"))",
            "assert(!verifySerializedJsonFile(\"candidate\",source))",
            "assert(peakBytes==before && liveBytes==before)",
            "assert(!atomicReplaceFromTmp(\"primary.tmp\",\"primary\",&source))",
            "assert(!storage.saveJSONConfigImpl(\"primary\",oom))",
            "assert(legacyRead[\"admin\"][\"username\"]==\"admin\")",
            "readLimit=limit",
            "readFailureAt=failure",
            "const bool saved=storage.flushToDisk(\"primary\",source)"
        ]
    }
},
{
    "name": "IPC permanent ownership uses current-task creation numbers without waiting or rereading dynamic handles",
    "files": {
        "test/unit/test_ipc_task_owner.py": [
            "class IpcTaskOwnerTests",
            "def test_only_later_explicit_post_retries_nonqueued_busy_slot",
            "def test_pending_not_reused_and_late_mapping_not_backfilled",
            "def test_wrong_cpu_identity_stays_unknown",
            "def test_duplicate_creation_numbers_stay_unknown",
            "def test_real_backend_snapshot_and_json_keep_original_watermarks",
            "def test_competing_submitters_and_readers_never_publish_torn_identity",
            "def test_c3_does_not_submit_or_allocate_ipc_storage",
            "def test_no_trace_does_not_submit_or_allocate_ipc_storage",
            "def test_real_route_observe_only_after_boot_submits_no_ipc",
            "def test_real_route_observe_only_reuses_ready_identity",
            "def test_real_route_observe_only_never_retries_rejected_context",
            "def test_real_route_rejects_unknown_empty_duplicate_and_file_parameters",
            "def test_real_route_observe_only_preserves_auth_and_developer_checks"
        ],
        "test/unit/cpp/ipc_task_owner_harness.cpp": [
            "task == nullptr && scan == pdFALSE",
            "int esp_ipc_call_blocking(uint32_t, void (*)(void*), void*) { std::abort(); }",
            "unsigned uxTaskGetTaskNumber(void*) { std::abort(); }",
            "highWaterBytes == 252",
            "round < 300",
            "IpcOwnerStatus::PublishedAfterCapture",
            "ipcCalls[0] == 1 && ipcCalls[1] == 1",
            "!ipcCalls[0] && !ipcCalls[1] && infoCalls == 0"
        ]
    }
},
{
    "name": "Log VFS operation deadlines reject genuine overruns and preserve historical negative controls",
    "files": {
        "test/unit/test_log_directory_budget.py": [
            "def test_actual_r6_batch_budget_reproduces_129ms_cumulative_failure",
            "def test_actual_individual_vfs_budget_accepts_129ms_and_wrap",
            "baseline='r6',operation_r6=True"
        ],
        "test/unit/cpp/log_directory_r6_before.inc": [
            "class LogDirectoryScanBudget",
            "void readLogList("
        ]
    }
},

{
    "name": "running image metadata uses bounded partition reads and fails visibly without payload verification",
    "files": {
        "test/unit/test_running_image_size.py": [
            "class RunningImageSizeTests",
            "test_unsigned_real_production_reader_four_chip_ids_bounds_failures_and_concurrency",
            "test_signed_boot_and_signed_app_configurations_explicitly_reject_unknown_size",
            "test_actual_pinned_rgb_binary_length_matches_independent_build_report",
            "test_full_compact_and_batch_only_emit_flash_after_successful_shared_reader",
            "src/systems/RunningImageSize.cpp",
            "@COMPACT_SEND@",
            "@BATCH_BRANCH@"
        ],
        "test/unit/cpp/running_image_size_harness.cpp.in": [
            "forbidHeap=true",
            "16u*1024u*1024u+1u",
            "0xfffffffcu",
            "failAt=call",
            "800*18",
            "800*200",
            "IMAGE_HEADER_READ_FAILED",
            "SIGNED_SIZE_UNKNOWN_PASS",
            "failedAllocation.connection.closed",
            "ACTUAL_BINARY_METADATA_PASS"
        ]
    }
},
{
    "name": "deferred log reads bound directory scans and recheck memory after contention before synchronous task reclamation",
    "files": {
        "test/unit/test_log_deferred_reader.py": [
            "class LogDeferredReaderTests",
            "def test_real_worker_admission_lock_disconnect_cleanup_and_retirement",
            "src/network/handlers/LogRouteHandler.cpp",
            "@WORKER@",
            "@GUARD@",
            "@DESTRUCTOR@"
        ],
        "test/unit/test_log_internal_allocation.py": [
            "class LogInternalAllocationTests",
            "def test_real_allocator_caps_and_cold_start_oom_before_constructor",
            "@ALLOCATE@",
            "@SIZED_DELETE@",
            "@INITIALIZE@"
        ],
        "test/unit/cpp/log_internal_allocation_harness.cpp.in": [
            "MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT",
            "allocationFails=true",
            "constructions==0",
            "routeRegistrations==0"
        ],
        "test/unit/test_log_directory_budget.py": [
            "class LogDirectoryBudgetTests",
            "def test_actual_directory_reader_bounds_and_resource_rechecks",
            "src/network/handlers/LogRouteHandler.cpp",
            "@READER@",
            "@BUDGET@",
            "@SCAN@"
        ],
        "test/unit/cpp/log_directory_budget_harness.cpp.in": [
            "reset(64)",
            "reset(65)",
            "reset(500)",
            "fileOpenElapsedMs=120",
            "fileOpenElapsedMs=121",
            "clockMs=UINT32_MAX-60",
            "pressureCall=2"
        ],
        "test/unit/cpp/log_deferred_reader_harness.cpp.in": [
            "deliverNotify=false",
            "LOGGER.files.lock()",
            "assert(!weak.expired()",
            "allowSuspend=false",
            "holdExternalDelete=true",
            "liveTaskAllocations==1",
            "FastBee::MemoryBudget::freeBytes=34000",
            "FastBee::MemoryBudget::freeBytes=1000",
            "request->ownedClient.rxTimeout==0",
            "request->ownedClient.rxTimeout==3",
            "assert(weak.expired())",
            "operations==34"
        ]
    }
},
{
    "name": "task stack SDK permanent owner matching never dereferences dynamic handles",
    "files": {
        "test/unit/test_task_stack_snapshot.py": [
            "def test_permanent_owners_and_creation_numbers",
            "def test_single_core_never_queries_idle1",
            "def test_null_timer_remains_unknown",
            "def test_scheduler_not_started_never_calls_permanent_getters",
            "def test_smp_idle_api_unavailable_is_honest",
            "def test_dangling_dynamic_handle_is_not_dereferenced",
            "def test_full_scan_callers_are_explicit_post_and_gated_health_sampler"
        ],
        "test/unit/cpp/task_stack_snapshot_harness.cpp.in": [
            "pcTaskGetName(void*) { std::abort(); }",
            "uxTaskGetTaskNumber(void*) { std::abort(); }"
        ]
    }
},
{
    "name": "periodic full task-stack sampler is interval-gated observe-only and zero-allocation",
    "files": {
        "test/unit/test_task_stack_periodic_sampling.py": [
            "class TaskStackPeriodicSamplingTests",
            "def test_interval_gate_fires_once_per_window",
            "def test_millis_wraparound_keeps_interval_gate",
            "def test_low_watermark_tasks_reported_with_severity",
            "def test_memory_pressure_skips_scan_and_advances_gate",
            "def test_sampler_source_is_observe_only_zero_alloc_and_gated"
        ],
        "test/unit/cpp/task_stack_periodic_sampling_harness.cpp.in": [
            "assert(!explicitRequest)",
            "assert(INTERVAL >= 300000UL && INTERVAL <= 600000UL);",
            "INTERNAL_MEMORY_BUDGET",
            "assert(sdkCalls == 0 && monitor.lastFullStackLogTime == 0)"
        ]
    }
},
{
    "name": "public status entry activity survives cached and rejection branches",
    "files": {
        "test/unit/test_system_status_feature_intent.py": [
            "def test_entry_activity_covers_cache_fresh_and_rejected_responses",
            "def test_original_missing_activity_is_rejected",
            "def test_cache_only_activity_is_rejected"
        ]
    }
},
{
    "name": "catalog per-driver streaming preserves full wire shape and OOM cursor",
    "files": {
        "test/unit/test_peripheral_catalog_response.py": [
            "def test_full_catalog_equivalence_memory_and_response_lifetime",
            "036561c1f3d2fd1b8f0416a961a92914fe1efad28ca677eee7afb24965c6556c"
        ],
        "test/unit/cpp/peripheral_catalog_response_harness.cpp": [
            "streamedSensorDescriptor",
            "Reader::TryAgain",
            "catalogAllocator.failAllocations = true",
            "Captured feature state cannot change halfway through a response."
        ]
    }
},
    {
        name: 'configuration load transfers verified DOM with caller allocator and preserves valid data on failure',
        files: {
            'test/unit/test_config_storage_load_ownership.py': [
                'class ConfigStorageLoadOwnershipTests', 'def test_large_json_peak_failure_preservation_and_custom_allocator'
            ],
            'test/unit/cpp/config_storage_load_ownership_harness.cpp.in': [
                '@PRODUCTION@', '@ORACLE@', 'customAllocator.fail=true', 'ownershipPeak < copyPeak', 'out.allocator()==&customAllocator'
            ],
            'test/unit/cpp/config_storage_load_before.inc': ['bool verifyJsonFileBefore(', '*out = parsed']
        }
    },
    {
        name: 'script shutdown drains readers and checked rule loading preserves valid configurations',
        files: {
            'test/unit/test_rule_script_lifecycle.py': [
                'class RuleScriptLifecycleTests', 'retire_with_reader', 'concurrent_retirement', 'cancel_initialization'
            ],
            'test/unit/test_rule_script_import.py': [
                'class RuleScriptImportTests', 'node_allocation_failure', 'load_json_allocation_failure',
                'memory_rejection', 'startup_limits', 'structural_backup_recovery', 'legacy_defaults'
            ],
            'test/unit/cpp/rule_script_import_harness.cpp.in': [
                'core/RuleScriptResources.h', '__wrap_malloc', 'failStringAt', 'LittleFS.backupRaw'
            ]
        }
    },
    {
        name: 'OTA reply queues allocate on demand and retire after the last admitted caller',
        files: {
            'test/unit/test_ota_reply_queue_lifetime.py': [
                'def test_actual_owner_cold_oom_inflight_retirement_and_concurrency',
                'def test_flash_admission_and_cleanup_keep_reply_ownership'
            ],
            'test/unit/cpp/ota_reply_queue_lifetime_harness.cpp': [
                'OtaReplyQueueOwner', 'failQueueCall', 'detachedLease',
                'continueCreate', 'Queued terminal/reply/progress'
            ]
        }
    },
    {
        name: 'recent log optional buffer survives admitted readers and allocation cancellation',
        files: {
            'test/unit/test_logger_recent_runtime.py': [
                'def test_recent_buffer_lifetime_with_real_snapshot_and_disable'
            ],
            'test/unit/cpp/logger_recent_runtime_harness.cpp': [
                'in-flight-reader', 'critical-survives', 'duringAllocation', 'releaseBefore'
            ]
        }
    },
    {
        name: 'sensor cache allocates actual entries on demand and invalidates cooperative readers',
        files: {
            'test/unit/test_sensor_cache_demand.py': [
                'class SensorCacheDemandTests',
                'def test_actual_cache_cold_oom_release_eviction_and_cooperative_replacement'
            ],
            'test/unit/cpp/sensor_cache_demand_harness.cpp': [
                'SENSOR_DRIVER_MEMORY_UNAVAILABLE', 'releaseAll', 'sensor cache demand lifecycle checks passed'
            ]
        }
    },
    {
        name: 'observer exceptions preserve private originals and public failure judgments',
        files: {
            'test/unit/test_safe_observer_error.py': [
                'def test_real_checkers_keep_cookie_errors_private_and_remain_failed',
                'test/unit/test_safe_observer_error.mjs'
            ],
            'test/unit/test_safe_observer_error.mjs': [
                'scripts/test-web-device-menus.mjs', 'scripts/test-com15-monitor-device.mjs',
                'collectWorkerErrors', 'finalizeMonitorRun', 'privateCaptureFailed',
                'privateSha256', 'publicEventsDropped'
            ]
        }
    },
    {
        name: 'feature status immutable snapshots and allocation-safe finite streaming',
        files: {
            'test/unit/test_feature_network_fixed_json.py': [
                'class FeatureStatusCatalogTests',
                'def test_snapshot_matches_existing_schema_and_never_rereads_live_state',
                'def test_small_slices_disconnect_and_preflight_reject_oversize_or_oom'
            ],
            'test/unit/cpp/feature_status_catalog_harness.cpp': [
                'FeatureStatusCatalog', 'JsonVariantConst', 'rejectArray', 'arrayBudget = 1',
                'changed after capture', 'oversized.measureLength()', 'allocator.live.empty()'
            ]
        }
    },
    {
        name: 'log route waits for concurrent flush and preserves lock ownership',
        files: {
            'test/unit/test_log_route_contention.py': [
                'def test_read_waits_for_flush_and_timeout_does_not_unlock_writer',
                'src/network/handlers/LogRouteHandler.cpp',
                'std::timed_mutex', 'assert(LOGGER.unlocks==1)'
            ]
        }
    },
    {
        name: 'S3 fixed slab actual storage diagnostics',
        files: {
            'test/unit/test_static_slab_storage_diagnostics.py': [
                'def test_actual_serializer_external_fallback_absence_and_inline_profiles',
                'src/network/handlers/SystemRouteHandler.cpp'
            ],
            'test/unit/cpp/static_slab_storage_diagnostics_harness.cpp.in': [
                '@PRODUCTION_POOL_SERIALIZER@', 'ACTUAL_POOL_SERIALIZER_PASS',
                'pools.size()==3', 'pool["externalStorage"]==external'
            ]
        }
    },
    {
        name: 'S3 worker budget and actual pool lifetime',
        files: {
            'test/unit/test_worker_budget_policy.py': ['def test_actual_policy_pool_exit_growth_and_network_baseline', 'src/core/PeriphExecWorkers.cpp'],
            'test/unit/cpp/worker_budget_harness.cpp.in': [
                '@POLICY@', '@POOL_HEADER@', '@POOL_SOURCE@', '@LIFETIME@', '@NETWORK_API@', '@BASELINE@', '@ADMISSION@',
                'Partial begin failure never frees', 'Partial sentinel send retries', 'UINT32_MAX-999',
                'A later fresh lease cannot replace the failed outer admission'
            ]
        }
    },
    {
        name: 'historical raw device metrics contract and HTTP completeness',
        files: {
            'scripts/device_metrics_contract.py': ['def raw_metrics(body):', 'def metrics_response(response):'],
            'test/unit/test_device_metrics_contract.py': [
                'def test_real_historical_raw_shape_and_complete_http_body',
                'def test_missing_groups_and_scalar_fields_cannot_fabricate_metrics',
                'def test_error_and_explicit_failure_envelopes_never_use_raw_fallback',
                'def test_truncated_invalid_length_or_http_failure_is_rejected',
                'def test_other_api_routes_keep_strict_success_and_existing_data_semantics'
            ]
        }
    },
    {
        name: 'fixed slab PSRAM storage capability and ownership',
        files: {
            'test/unit/test_static_slab_storage.py': [
                'def test_external_storage_fallback_ownership_and_lifetime',
                'def test_inline_profiles_and_missing_board_support'
            ],
            'test/unit/cpp/static_slab_storage_harness.cpp': [
                '#include "utils/StaticPoolAllocator.h"',
                'GLOBAL_DESTRUCTOR_SAFE', 'pool.deallocate(static_cast<unsigned char*>(a)+1)',
                '!Failed::pool().storageAvailable()'
            ]
        }
    },
    {
        name: 'web handler literal parameter bridge',
        files: {
            'test/unit/test_web_handler_parameter_bridge.py': [
                'def test_actual_methods_preserve_post_query_null_empty_and_mixed_overloads',
                'def test_literal_bridges_keep_shared_string_paths_and_compiler_boundaries'
            ],
            'test/unit/cpp/web_handler_parameter_bridge_harness.cpp.in': ['@PRODUCTION_METHODS@', '@ORIGINAL_METHODS@'],
            'test/unit/cpp/web_handler_parameter_before.json': ['declarations', 'definitions']
        }
    },
    {
        name: 'web handler literal response bridge',
        files: {
            'test/unit/test_web_handler_response_bridge.py': [
                'def test_actual_response_methods_preserve_status_escaping_truncation_and_headers',
                'def test_three_literal_bridges_reuse_original_string_and_document_response_methods',
                'PLATFORMIO_LIBDEPS_DIR'
            ],
            'test/unit/cpp/web_handler_response_bridge_harness.cpp.in': ['@PRODUCTION_METHODS@', '@ORIGINAL_METHODS@'],
            'test/unit/cpp/web_handler_response_before.json': ['declarations', 'definitions']
        }
    },
    {
        name: 'shared literal String case match',
        files: {
            'test/unit/test_literal_string_case_match.py': [
                'def test_actual_case_match_preserves_null_empty_case_utf8_and_long_inputs',
                "ROOT/'src/network/LiteralStringMatch.cpp'"
            ],
            'test/unit/cpp/literal_string_case_match_harness.cpp.in': [
                '#include "network/LiteralStringMatch.h"',
                'Public case match declaration changed',
                'value.equalsIgnoreCase(String(literal.c_str()))'
            ],
            'test/unit/cpp/literal_string_case_match_before.json': ['comparisonCases', 'nullLiteralComparisons', 'arrays']
        }
    },
    {
        name: 'modbus observation worker stack and online admission',
        files: {
            'test/unit/test_modbus_worker_stack.py': [
                'def test_fixed_format_matches_printf_without_newlib_formatting',
                'def test_production_observation_dispatch_preserves_wire_and_quality_without_parse'
            ],
            'test/unit/cpp/modbus_fixed_format_harness.cpp': ['formatModbusFixed'],
            'test/unit/cpp/observation_direct_dispatch_harness.cpp': ['production_direct_dispatch.inc'],
            'test/unit/test_worker_online_admission.py': ['def test_real_gate_preserves_online_web_budget_and_profile_behavior'],
            'test/unit/test_worker_demand_lifecycle.py': [
                'def test_production_demand_suspend_recovery_and_concurrent_creation',
                'def test_dispatch_checks_worker_admission_before_operation_or_reservation'
            ]
        }
    },
    {
        name: 'modbus hex capture worker stack',
        files: {
            'test/unit/test_modbus_hex_capture.py': [
                'def test_actual_capture_preserves_bytes_spacing_and_heap_guards',
                'def test_oracle_rejects_capture_regressions',
                'def test_original_formatter_is_rejected_at_compile_time'
            ],
            'test/unit/cpp/modbus_hex_capture_harness.cpp.in': [
                '#define snprintf FASTBEE_FORBIDDEN_snprintf',
                '@TX_BLOCK@',
                '@RX_BLOCK@',
                'tx heapSafe false keeps both',
                'rx heapSafe false keeps both',
                'size_t(254), size_t(255), size_t(256)'
            ]
        }
    },
    {
        name: 'feature menu gating',
        files: {
            'test/unit/test_menu_feature_contract.py': [
                'def test_only_optional_pages_are_feature_gated',
                'def test_removed_file_manager_page_has_no_web_route_or_test_project',
                'def test_authenticated_status_requires_compiled_and_active',
                'def test_status_failures_do_not_restore_all_menus',
                'def test_fleet_quick_includes_live_menu_visibility_regression',
                'def test_browser_navigation_rejects_hidden_preloaded_pages'
            ],
            'test/unit/test_menu_feature_runtime.py': [
                'def test_compact_panel_intent_avoids_full_catalog_for_all_runtime_states',
                'def test_incomplete_or_non_boolean_intent_retains_full_status_fallback',
                'def test_compact_intent_cannot_replace_missing_runtime_status',
                'def test_no_path_marks_feature_state_ready_before_its_response'
            ],
            'test/unit/test_system_status_feature_intent.py': [
                'def test_compiled_and_user_intent_survive_admission_changes',
                'def test_absent_panel_does_not_fabricate_intent_or_change_other_features',
                'def test_allocation_limits_reject_overflow_instead_of_valid_status'
            ],
            'test/unit/cpp/system_status_feature_intent_harness.cpp': [
                '#include "system_status_feature_block.inc"',
                '#include "system_status_feature_methods.inc"'
            ]
        }
    },
    {
        name: 'memory degradation and recovery',
        files: {
            'scripts/test-memory-regressions.js': [
                "setRuntimeDisabled(\"fileLogging\"",
                'PeriphExec must reject heavy event rules during severe memory pressure',
                'Memory protection must pause/retry TLS without rewriting user scheme/port/enabled or permitting plaintext fallback'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_classic_no_psram_disables_mqtt_before_staging',
                'def test_mqtt_degradation_is_reported_separately_from_runtime_features',
                'def test_memory_profile_prunes_only_profile_disabled_runtime_entries'
            ],
            'test/test_memory_budget.cpp': [
                'void test_guard_level_uses_internal_dram_budgets()',
                'void test_periph_exec_resource_guard()'
            ]
        }
    },
    {
        name: 'mqtt failure feedback',
        files: {
            'test/unit/test_mqtt_status_visibility.py': [
                'def test_readable_error_message_is_exposed_to_ui',
                'def test_http_503_memory_reason_is_actionable',
                'def test_browser_network_error_is_translated'
            ],
            'test/browser/suites/25-mqtt-status-feedback.spec.ts': [
                'MQTT-003B: 首次状态返回前显示中性检测态 @quick',
                'MQTT-003A: 连接失败原因明确显示且不修改配置 @quick',
                'MQTT-003D: 旧状态响应不得覆盖新页面结果 @quick'
            ]
        }
    },
    {
        name: 'analog and digital input conversion',
        files: {
            'test/unit/test_analog_input_contract.py': [
                'def test_ai_conversion_fields_are_exposed_and_submitted',
                'def test_ai_conversion_uses_guarded_linear_mapping',
                'def test_di_options_round_trip_and_simple_defaults',
                'def test_di_report_modes_have_distinct_runtime_paths'
            ],
            'test/test_peripheral_manager_opt.cpp': [
                'void test_analog_raw_conversion_boundaries()',
                'void test_analog_output_percent_mapping_uses_production_helper()'
            ],
            'test/browser/suites/05-peripheral.spec.ts': [
                'PER-083: AI和DI参数新增保存、编辑回填及清理闭环'
            ]
        }
    },
    {
        name: 'five-device independent configuration',
        files: {
            'scripts/validate-device-config-examples.js': [
                "environment: 'esp32c3-F4R0'",
                "environment: 'esp32c6-F4R0'",
                "environment: 'esp32-F4R0'",
                "environment: 'esp32s3-F16R8'",
                "environment: 'esp32s3-F8R0'",
                'configuration is identical to'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_all_five_devices_have_unique_serial_targets',
                'def test_fleet_refuses_identical_configuration_sources',
                'def test_repository_examples_are_the_default_configuration_sources'
            ]
        }
    },
    {
        name: 'configuration transfer roundtrip',
        files: {
            'test/unit/test_config_transfer_contract.py': [
                'def test_live_smoke_imports_the_just_exported_document',
                'def test_import_success_checks_envelope_and_returned_file_name',
                'def test_import_requires_session_and_does_not_pass_degraded_or_unknown_write'
            ],
            'test/test_system_services.cpp': [
                'static void test_config_import_valid_json()',
                'static void test_config_transfer_import_selective()'
            ]
        }
    },
    {
        name: 'feature persistence and factory reset durability',
        files: {
            'test/unit/test_feature_config_persistence_contract.py': [
                'def test_registry_factory_config_and_fixture_have_identical_keys',
                'def test_manager_writes_full_snapshot_and_normalizes_sparse_files',
                'def test_network_mdns_compatibility_field_is_synchronized_to_features'
            ],
            'test/unit/test_network_mdns_feature_contract.py': [
                'def test_sparse_snapshot_keeps_missing_mdns_eligible_for_legacy_migration',
                'def test_explicit_features_mdns_overrides_stale_network_mirror',
                'def test_update_rolls_back_when_either_config_file_save_fails',
                'def test_uncompiled_mdns_enable_is_rejected_and_runtime_gate_stays_closed'
            ],
            'test/unit/test_factory_reset_persistence_contract.py': [
                'def test_factory_write_validates_and_removes_older_generations',
                'def test_web_reset_covers_every_known_json_config',
                'def test_web_reset_reports_partial_failure_without_scheduling_reboot',
                'def test_rule_triggered_reset_aborts_reboot_on_any_cleanup_failure'
            ]
        }
    },
    {
        name: 'static resource transport integrity',
        files: {
            'test/unit/test_libdeps_patch_contract.py': [
                'def test_zero_read_fix_installs_upgrades_and_is_idempotent',
                'def test_send_progress_accounting_installs_and_is_idempotent',
                'def test_installed_312_copies_are_patched_when_available',
                'def test_validator_requires_complete_four_file_finite_progress_contract',
                'def test_validator_installed_sources_require_finite_progress_gate'
            ],
            'test/unit/test_web_finite_progress.py': [
                'def test_actual_finite_progress_paths',
                'def test_original_and_false_progress_mutations_are_detected',
                'def test_partial_progress_upgrades_are_rejected',
                'def test_four_file_partial_install_and_write_failure_preserve_originals',
                'def test_fixed_pre_finite_fixture_upgrades_and_is_idempotent'
            ],
            'test/unit/cpp/web_finite_progress_harness.cpp': [
                'void stalled(',
                'void zeroSpacePollAndTimeout()',
                'void setupAndRespond()',
                'void repeatedAckAndPendingOutput()',
                'void recoveryAndSlowProgress()',
                'void peerHighWaterAndFinalFlush()',
                'void excludedTypes()',
                'void releaseOwnershipAndLease()',
                '#include "web_peer_drain_harness.cpp"',
                '#include "production_transport_clock.inc"',
                '#include "production_owned_request.inc"',
                '#include "production_file_destructor.inc"',
                '#include "production_lease_types.inc"',
                '#include "production_lease_release.inc"',
                'int main()',
                'stalled(true,0);stalled(false,0);stalled(false,UINT32_MAX-5000U);',
                'zeroSpacePollAndTimeout();setupAndRespond();repeatedAckAndPendingOutput();',
                'recoveryAndSlowProgress();peerHighWaterAndFinalFlush();excludedTypes();',
                'releaseOwnershipAndLease();',
                'assert(liveArrays==0);',
                'assert(TransportClock::_fastbee_accepted_count==0);'
            ],
            'test/test_static_concurrency.cpp': [
                'void test_page_loader_response_integrity_locked()',
                'void test_limiter_no_psram_adaptive_behavior()',
                'void test_static_transfer_active_reaps_expired_leases()',
                'void test_limiter_millis_wraparound_safe()',
                'void test_limiter_staggered_expiry_isolated()',
                'void test_limiter_stale_disconnect_generation_safe()'
            ],
            'test/browser/suites/24-web-smooth-access.spec.ts': [
                'WEB-001: 首页启动和关键静态资源完整加载 @quick',
                'WEB-002: 连续刷新后页面仍可用 @quick',
                'WEB-004: HTTP 失败诊断保留原因并脱敏 @quick',
                "durationLimitFromEnv('WEB_ROOT_LOAD_MAX_MS', 10_000)",
                "durationLimitFromEnv('WEB_REFRESH_LOAD_MAX_MS', 10_000)",
                "durationLimitFromEnv('WEB_ROOT_P95_MAX_MS', 8_000)"
            ]
        }
    },
    {
        name: 'web interaction quality',
        files: {
            'test/browser/suites/03-network.spec.ts': [
                'NET-003A: 状态请求返回前保持中性检测态 @quick'
            ],
            'test/browser/suites/15-ui-regression.spec.ts': [
                'UI-005A: 360/390/430 管理表格无横向溢出',
                'UI-005C: OTA 和配置导入复用统一文件选择控件',
                'UI-005D: 窄屏 CRUD 表格与操作菜单不产生横向溢出',
                'UI-005E: 所有可见菜单页在 360/390/430 下无页面溢出且 DOM 有界',
                'UI-005F: 360/390/430 设备配置页键值表无横向溢出',
                'UI-005G: 设备配置 Tab 键盘可达与 ARIA 语义',
                'UI-007A: 模态框连续开关 20 次关闭即销毁且 DOM 不增长',
                'UI-007B: Modbus 分片弹窗按需挂载并在关闭后销毁',
                'UI-007C: 模态模板未预取时首次点击会续接且并发只请求一次',
                'UI-007D: 360px 长动态内容模态框无横向溢出 @quick',
                'UI-007E: 模态模板延迟返回时切页不得挂载旧页面弹窗 @quick',
                'UI-025A: 重复切页不累积隐藏 DOM 且回访按钮可用',
                'UI-025B: 重复进入设备大屏不累积 document 监听器 @quick',
                'UI-031: 模态框键盘闭环与 ARIA 属性 @quick'
            ],
            'test/browser/suites/22-feature-config.spec.ts': [
                'FCFG-004A: 功能状态资源依赖和开关同一行直接显示 @quick'
            ],
            'test/browser/suites/25-mqtt-status-feedback.spec.ts': [
                'MQTT-003C: 连续状态请求失败退出骨架并显示失败 @quick'
            ]
        }
    },
    {
        name: 'authentication session reuse and safety',
        files: {
            'test/unit/test_auth_session_contract.py': [
                'def test_invalid_session_copies_username_before_erasing_map_entry',
                'def test_browser_health_checks_do_not_bypass_cached_session_restore',
                'def test_browser_login_retries_follow_server_retry_after',
                'def test_browser_login_failure_diagnostics_are_attached_and_redacted'
            ]
        }
    },
    {
        name: 'deployment workflow safety',
        files: {
            'scripts/test-deploy-pio-observability.js': [
                'all deploy.ps1 PIO invocations must pass through the single logging wrapper',
                'web asset generation must never trigger an implicit upload or monitor'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_staging_excludes_repository_files_not_in_selected_bundle',
                'def test_littlefs_only_mode_is_forwarded_and_audited',
                'def test_sta_credentials_are_injected_at_deployment_time'
            ],
            'scripts/test-test-all-contract.js': [
                'device smoke without BaseUrl must fail',
                'device AP address must fail'
            ]
        }
    },
    {
        name: 'device discovery parsing and deduplication',
        files: {
            'scripts/test-device-discovery.js': [
                'the same device must be merged across passes and MAC casing',
                'the latest pass must replace stale device data',
                'MAC prefix filtering must ignore case and separator style',
                'separator-less 12-hex and prefix MAC filters must match the colon-separated device MAC'
            ]
        }
    },
    {
        name: 'screen panel status text maps error codes and network values to Chinese',
        files: {
            'test/unit/test_screen_panel_status_text.py': [
                'class ScreenPanelStatusTextTests',
                'def test_async_exec_error_codes_map_to_chinese',
                'def test_network_status_values_map_to_chinese',
                'def test_unknown_machine_codes_fall_back_to_readable_text',
                'def test_human_readable_text_passthrough',
                'def test_renderer_and_default_page_use_the_mapping'
            ],
            'include/screen/PanelStatusText.h': [
                'inline const char* panelErrorText(const char* error)',
                'inline const char* panelNetworkStatusText(const char* status)',
                'return machineCode ? "未知错误" : error;',
                'return machineCode ? "未知状态" : status;'
            ]
        }
    },
    {
        name: 'peripheral configuration workspace scope',
        files: {
            'test/unit/test_p1_peripheral_web_contract.py': [
                'def test_workspace_only_exposes_peripheral_configuration',
                'self.assertNotIn("peripheral-board", WEB)',
                'self.assertNotIn("peripheral-experiments", STATE)'
            ],
            'test/browser/suites/15-ui-regression.spec.ts': [
                'UI-026A: 外设配置只展示外设配置'
            ]
        }
    },
    {
        name: 'modbus oneshot structured results stay inside the fixed 32-slot buffer',
        files: {
            'test/unit/test_modbus_oneshot_bounds.py': [
                'class ModbusOneShotBoundsTests',
                'def test_production_oneshot_bounds_matrix',
                'OneShotResult ModbusHandler::readRegistersOnce(',
                'modbus_oneshot_bounds_production.inc'
            ],
            'test/unit/cpp/modbus_oneshot_bounds_harness.cpp': [
                'FW-P0-01',
                'uint16_t before[16];',
                '0xA5A5',
                'raw.length() == static_cast<size_t>((125 * 2 + 5) * 2)',
                '_readRangeInto(8, 3, 0, 125, box.buf, 125, 0, 0xFF)',
                'semaphoreAvailable = false',
                'MODBUS_EX_ILLEGAL_DATA_VALUE'
            ]
        }
    },
    {
        name: 'point history periodic flush coalesces into the logger flash write window with wrap-safe timing',
        files: {
            'include/core/PointHistory.h': [
                'FLUSH_DEADLINE_MS = 180000',
                'FLUSH_COALESCE_MIN_MS = 120000',
                'PEER_WRITE_WINDOW_MS = 2000'
            ],
            'src/core/PointHistory.cpp': [
                'LoggerSystem::getInstance().lastFlashWriteMs()',
                'elapsed(now, _firstDirtyAt, FLUSH_COALESCE_MIN_MS)'
            ],
            'include/systems/LoggerSystem.h': [
                'uint32_t lastFlashWriteMs() const',
                'std::atomic<uint32_t> _lastFlashWriteMs{0}'
            ],
            'src/systems/LoggerSystem.cpp': ['_lastFlashWriteMs.store(_ringBuffer->lastFlushMs)'],
            'test/unit/test_point_history_persistence.py': [
                'def test_dirty_data_rides_recent_log_flush_only_inside_coalesce_window',
                'def test_flush_windows_survive_millis_wraparound'
            ],
            'test/unit/cpp/point_history_persistence_harness.cpp': [
                'static void coalesce()',
                'static void wraparound()',
                'uint32_t lastFlashWriteMs() const { return peerFlashWriteMs; }'
            ]
        }
    },
    {
        name: 'screen large-font tiers use wqy16 at rect.h>=120 and degrade pixel-exact without the face',
        files: {
            'src/screen/StripeRenderer.h': [
                'static constexpr uint8_t renderedTier(uint8_t scale)',
                'if (large) font = u8g2_font_wqy16_t_gb2312;',
                'rect.h >= 120 ? 3 : rect.h >= 74 ? 2 : 1',
                'rect.h >= 120 ? 4 : 1'
            ],
            'include/core/FeatureFlags.h': ['#define FASTBEE_SCREEN_LARGE_FONT 0'],
            'platformio.ini': ['-DFASTBEE_SCREEN_LARGE_FONT=1'],
            'test/unit/test_screen_font_raster.py': [
                'for(int scale : {1,2,3,4})',
                '-DFASTBEE_SCREEN_LARGE_FONT=1", "-DHARNESS_LARGE_FONT=1'
            ],
            'test/unit/test_screen_renderer_raster.py': [
                'void largeFont()',
                'def test_large_font_tiers_thresholds_and_stripe_compatibility'
            ]
        }
    },
    {
        name: 'motor adjust and numeric plus/minus hold auto-repeat cadence, boost and stop paths',
        files: {
            'test/unit/test_touch_screen_interactions.py': [
                'def test_plus_minus_hold_auto_repeat',
                'REPEAT_START_MS=600,REPEAT_INTERVAL_MS=150,REPEAT_FAST_AFTER_MS=2000',
                'assert(c.motorPulseInput==100+10*100+4*1000&&taps==0);'
            ],
            'src/screen/ScreenRuntime.cpp': [
                'constexpr uint32_t REPEAT_START_MS = 600;',
                'gesture.repeatNextMs += REPEAT_INTERVAL_MS;',
                '!gesture.tapCancelled && !gesture.repeated && gesture.tapTarget'
            ]
        }
    },
    {
        name: 'unused CSS rule stripping keeps referenced selectors and trims capability-excluded styles',
        files: {
            'scripts/test-web-css-strip.js': [
                'stripUnreferencedCssRules',
                'fixture must keep',
                'fixture must strip',
                'stripped main.css must keep ${needle}',
                '@keyframes cardAppear'
            ],
            'scripts/build-web-modules.js': [
                'function getCssReferenceCorpus',
                'RUNTIME_SUB_DIR_CAPABILITY_RULES',
                'CSS strip (${relPath})'
            ]
        }
    },
    {
        name: 'MQTT publish path uses fixed slot/pool buffers without whole-payload String copies',
        files: {
            'test/unit/test_mqtt_publish_staging_contract.py': [
                'class MqttPublishStagingContractTests',
                'def test_flush_path_publishes_slot_bytes_without_string_copy',
                'def test_staging_buffer_is_static_pool_guarded_not_task_stack',
                'def test_oversize_payload_rejected_not_truncated'
            ],
            'src/protocols/MQTTClient.cpp': [
                'const char* pendingBytes = _pendingReportSlot.payload;',
                'struct MqttPublishBufferGuard',
                'const bool bypassTransform = prepared ||'
            ],
            'test/unit/cpp/mqtt_report_queue_harness.cpp.in': ['if(mode=="consecutive_distinct")']
        }
    },
    {
        name: 'P0 peripheral setup reaches real initializers for the five previously unreachable types and fails closed otherwise',
        files: {
            'test/unit/test_peripheral_lifecycle_capability_contract.py': [
                'class PeripheralLifecycleCapabilityContractTests',
                'def test_setup_reaches_real_initializers_instead_of_teardown',
                'def test_teardown_is_symmetric_for_special_peripherals',
                'def test_touch_fails_closed_until_real_semantics_exist'
            ]
        }
    },
    {
        name: 'P0 protocol hot-restart lifecycle uses leases and deferred destroy instead of use-after-free',
        files: {
            'test/unit/test_mqtt_client_lease.py': [
                'class MqttClientLeaseTests',
                'class MqttClientLeaseIntegrationTests',
                'def test_busy_restart_cannot_trigger_reboot_fallback',
                'def test_all_production_callers_use_scoped_admission'
            ],
            'test/unit/test_modbus_memory_recovery.py': [
                'class ModbusMemoryRecoveryTests',
                'bool ProtocolManager::restartModbusDeferred()',
                'IdleNoEnabledTargets,DeferredBusy'
            ],
            'src/protocols/ProtocolManager.cpp': [
                'Modbus restart deferred; active handler lease',
                'ModbusRestartResult::DeferredBusy',
                'PeriphExecManager::getInstance().setModbusReadCallback(nullptr);',
                'modbusHandler.reset();'
            ]
        }
    },
    {
        name: 'P0 web XSS sinks stay text-only in dashboard, fullscreen and notification renderers',
        files: {
            'test/unit/test_web_security_ux_contract.py': [
                'class WebSecurityUxContractTests',
                'def test_dashboard_and_fullscreen_use_node_renderers_for_device_strings',
                'def test_notifications_have_no_html_mode_or_inner_html_sink',
                'def test_feature_developer_mode_renderer_does_not_replace_global_method'
            ],
            'test/browser/suites/27-web-security-lifecycle.spec.ts': [
                'WEB-SEC-001',
                'WEB-SEC-002',
                'WEB-SEC-003',
                'WEB-SEC-004',
                'data-web-xss'
            ]
        }
    },
    {
        name: 'P0 high-risk endpoints keep auth, developer-mode, confirmation and allowlist guards',
        files: {
            'test/unit/test_sec_p0_security_contract.py': [
                'class SecP0SecurityContractTests',
                'def test_periph_exec_run_requires_auth_and_developer_mode_before_execution',
                'def test_factory_reset_requires_auth_confirmation_and_password_before_mutation',
                'def test_transfer_endpoints_share_a_fixed_non_sensitive_allowlist',
                'def test_file_save_blocks_every_direct_config_write'
            ]
        }
    }
];

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function relative(filePath) {
    return path.relative(ROOT_DIR, filePath).replace(/\\/g, '/');
}

function unique(values) {
    return Array.from(new Set(values));
}

function duplicates(values) {
    const seen = new Set();
    const dupes = new Set();
    values.forEach((value) => {
        if (seen.has(value)) dupes.add(value);
        seen.add(value);
    });
    return Array.from(dupes).sort();
}

function lineNumber(text, index) {
    return text.slice(0, index).split(/\r?\n/).length;
}

function maskCppNonCode(source) {
    const output = source.split('');
    const mask = (start, end) => {
        for (let index = start; index < end; index += 1) {
            if (output[index] !== '\n' && output[index] !== '\r') output[index] = ' ';
        }
    };

    let index = 0;
    while (index < source.length) {
        if (source.startsWith('//', index)) {
            const end = source.indexOf('\n', index + 2);
            const stop = end < 0 ? source.length : end;
            mask(index, stop);
            index = stop;
            continue;
        }
        if (source.startsWith('/*', index)) {
            const end = source.indexOf('*/', index + 2);
            const stop = end < 0 ? source.length : end + 2;
            mask(index, stop);
            index = stop;
            continue;
        }
        if (source[index] === 'R' && source[index + 1] === '"') {
            const openParen = source.indexOf('(', index + 2);
            if (openParen >= 0 && openParen - index <= 18) {
                const delimiter = source.slice(index + 2, openParen);
                if (!/[\s()\\]/.test(delimiter)) {
                    const closeToken = `)${delimiter}"`;
                    const close = source.indexOf(closeToken, openParen + 1);
                    const stop = close < 0 ? source.length : close + closeToken.length;
                    mask(index, stop);
                    index = stop;
                    continue;
                }
            }
        }
        if (source[index] === '"' || source[index] === "'") {
            const quote = source[index];
            const start = index;
            index += 1;
            while (index < source.length) {
                if (source[index] === '\\') {
                    index += 2;
                    continue;
                }
                if (source[index] === quote) {
                    index += 1;
                    break;
                }
                index += 1;
            }
            mask(start, Math.min(index, source.length));
            continue;
        }
        index += 1;
    }
    return output.join('');
}

function findMatchingBrace(code, openingBrace) {
    let depth = 0;
    for (let index = openingBrace; index < code.length; index += 1) {
        if (code[index] === '{') depth += 1;
        if (code[index] === '}') {
            depth -= 1;
            if (depth === 0) return index;
        }
    }
    return -1;
}

function extractTestFunctions(source) {
    const code = maskCppNonCode(source);
    const pattern = /(?:^|\n)\s*(?:static\s+)?(?:inline\s+)?void\s+(test_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*(?:noexcept\s*)?\{/g;
    const functions = [];
    let match;
    while ((match = pattern.exec(code)) !== null) {
        const openingBrace = code.indexOf('{', match.index);
        const closingBrace = findMatchingBrace(code, openingBrace);
        functions.push({ name: match[1], start: match.index, openingBrace, closingBrace, complete: closingBrace >= 0 });
        if (closingBrace < 0) break;
    }
    return { code, functions };
}

function analyzeTestSource(source) {
    const { code, functions } = extractTestFunctions(source);
    const registrations = [];
    const pattern = /\bRUN_TEST\s*\(\s*(test_[A-Za-z0-9_]+)\s*\)/g;
    let match;
    while ((match = pattern.exec(code)) !== null) {
        const container = functions.find((fn) =>
            fn.complete && match.index > fn.openingBrace && match.index < fn.closingBrace);
        registrations.push({ name: match[1], index: match.index, container: container ? container.name : null });
    }
    return {
        code,
        functions,
        groups: functions.filter((fn) => fn.name.endsWith('_group')),
        tests: functions.filter((fn) => !fn.name.endsWith('_group')),
        registrations
    };
}

function listTestFiles(testDir = TEST_DIR, issues = []) {
    const files = [];
    const walk = (directory) => {
        fs.readdirSync(directory, { withFileTypes: true })
            .sort((left, right) => left.name.localeCompare(right.name))
            .forEach((entry) => {
                const fullPath = path.join(directory, entry.name);
                if (entry.isDirectory()) {
                    if (!EXCLUDED_TEST_DIRS.has(entry.name.toLowerCase())) walk(fullPath);
                    return;
                }
                if (!entry.isFile() || !/^test_.*\.cpp$/i.test(entry.name) || entry.name === 'test_main.cpp') return;
                if (!/^test_.*\.cpp$/.test(entry.name)) {
                    issues.push(`${relative(fullPath)}: native test file names must use lowercase test_ prefix`);
                    return;
                }
                files.push(fullPath);
            });
    };
    walk(testDir);
    return files.sort();
}

function validateTestAnalysis(source, analysis, rel, mainDecls, mainCalls, issues, summary, globalGroups) {
    const groupNames = analysis.groups.map((group) => group.name);
    const testNames = analysis.tests.map((test) => test.name);
    const registrationNames = analysis.registrations.map((registration) => registration.name);

    summary.files += 1;
    summary.groups += analysis.groups.length;
    summary.tests += analysis.registrations.length;

    analysis.functions.filter((fn) => !fn.complete).forEach((fn) => {
        issues.push(`${rel}:${lineNumber(source, fn.start)}: ${fn.name} has an unmatched opening brace`);
    });
    duplicates(groupNames).forEach((name) => issues.push(`${rel}: duplicate group definition ${name}`));
    duplicates(testNames).forEach((name) => issues.push(`${rel}: duplicate local test function ${name}`));
    duplicates(registrationNames).forEach((name) => issues.push(`${rel}: duplicate RUN_TEST(${name}) registration`));

    if (analysis.groups.length === 0) issues.push(`${rel}: missing test_*_group() entry point`);
    if (analysis.registrations.length === 0) issues.push(`${rel}: missing RUN_TEST(...) cases`);

    analysis.groups.forEach((group) => {
        if (!globalGroups.has(group.name)) globalGroups.set(group.name, []);
        globalGroups.get(group.name).push(rel);
        if (!mainDecls.has(group.name)) issues.push(`${rel}: ${group.name} is not declared in test/test_main.cpp`);
        if (!mainCalls.has(group.name)) issues.push(`${rel}: ${group.name} is not called from test/test_main.cpp`);
        if (!analysis.registrations.some((registration) => registration.container === group.name)) {
            issues.push(`${rel}:${lineNumber(source, group.start)}: ${group.name} has no RUN_TEST(...) calls in its body`);
        }
    });

    const localTests = new Set(testNames);
    analysis.registrations.forEach((registration) => {
        if (!registration.container || !registration.container.endsWith('_group')) {
            issues.push(`${rel}:${lineNumber(source, registration.index)}: RUN_TEST(${registration.name}) must be inside a test_*_group() body`);
        }
        if (!localTests.has(registration.name)) {
            issues.push(`${rel}:${lineNumber(source, registration.index)}: RUN_TEST(${registration.name}) has no local definition`);
        }
    });

    analysis.tests.forEach((test) => {
        if (!analysis.registrations.some((registration) => registration.name === test.name)) {
            issues.push(`${rel}:${lineNumber(source, test.start)}: ${test.name} is defined but not registered with RUN_TEST`);
        }
    });
}

function findMatches(text, regex, groupIndex = 1) {
    const values = [];
    let match;
    while ((match = regex.exec(text)) !== null) values.push(match[groupIndex]);
    return values;
}

function extractCiFirmwareEnvironments(ciText) {
    const lines = ciText.split(/\r?\n/);
    const jobStart = lines.findIndex((line) => /^  firmware-build:\s*$/.test(line));
    if (jobStart < 0) return [];
    const jobEndOffset = lines.slice(jobStart + 1).findIndex((line) => /^  [A-Za-z0-9_-]+:\s*$/.test(line));
    const jobEnd = jobEndOffset < 0 ? lines.length : jobStart + 1 + jobEndOffset;
    const environmentLine = lines.findIndex((line, index) =>
        index > jobStart && index < jobEnd && /^\s+environment:\s*$/.test(line));
    if (environmentLine < 0) return [];
    const indent = lines[environmentLine].match(/^\s*/)[0].length;
    const environments = [];
    for (let index = environmentLine + 1; index < jobEnd; index += 1) {
        const line = lines[index];
        if (!line.trim()) continue;
        const lineIndent = line.match(/^\s*/)[0].length;
        if (lineIndent <= indent) break;
        const match = line.match(/^\s*-\s+([A-Za-z0-9_-]+)\s*$/);
        if (match) environments.push(match[1]);
    }
    return environments;
}

function validateSameSet(label, actual, expected, issues) {
    duplicates(actual).forEach((value) => issues.push(`${label}: duplicate ${value}`));
    const actualSet = new Set(actual);
    const expectedSet = new Set(expected);
    expectedSet.forEach((value) => {
        if (!actualSet.has(value)) issues.push(`${label}: missing ${value}`);
    });
    actualSet.forEach((value) => {
        if (!expectedSet.has(value)) issues.push(`${label}: unexpected ${value}`);
    });
}

function validateFirmwareMatrix(issues) {
    const platformioText = readText(PLATFORMIO_FILE);
    const platformioEnvs = findMatches(platformioText, /^\[env:([^\]]+)\]\s*$/gm)
        .filter((name) => name !== 'native');
    const ciText = readText(CI_FILE);
    const ciEnvs = extractCiFirmwareEnvironments(ciText);
    if (ciEnvs.length === 0) issues.push('.github/workflows/ci.yml: firmware-build matrix is missing or empty');
    validateSameSet('.github/workflows/ci.yml firmware-build matrix', ciEnvs, platformioEnvs, issues);
    if (!ciText.includes('pio run -e ${{ matrix.environment }} --target buildfs')) {
        issues.push('.github/workflows/ci.yml: firmware matrix must build LittleFS for every environment');
    }
    if (!ciText.includes('test-build-artifacts.js --env=${{ matrix.environment }} --current-build')) {
        issues.push('.github/workflows/ci.yml: firmware matrix must validate each current build artifact set');
    }
    [
        'node scripts/test-web-profile.js',
        'node scripts/test-web-capability-build.js',
        'node scripts/gzip-www.js --no-upload --no-monitor --env=esp32s3-F16R8 --stage-dir=.pio/ci-fs-stage',
        'test -f data/www/index.html.gz',
        'test -f data/www/js/chunk-1-core.js.gz',
        'test -f data/www/js/chunk-2-app.js.gz',
        'test -f data/www/js/chunk-3-state.js.gz',
        'test -f data/www/js/modules/dashboard-fullscreen.js.gz',
        'test -f .pio/ci-fs-stage/www/index.html.gz',
        'find data/www -type f',
        'find .pio/ci-fs-stage/www -type f',
        'node scripts/test-web-quality-gate.js',
        'node scripts/web-smoke-test.js',
        'node scripts/test-gzip-www-contract.js --stage-dir=.pio/ci-fs-stage'
    ].forEach((command) => {
        if (!ciText.includes(command)) {
            issues.push(`.github/workflows/ci.yml: web build is missing current output check: ${command}`);
        }
    });
}

function validateCriticalRegressionContracts(issues, readFile = (rel) => readText(path.join(ROOT_DIR, rel))) {
    CRITICAL_REGRESSION_CONTRACTS.forEach((contract) => {
        Object.entries(contract.files).forEach(([rel, markers]) => {
            let source;
            try {
                source = readFile(rel);
            } catch (error) {
                issues.push(`critical regression ${contract.name}: missing ${rel}`);
                return;
            }
            markers.forEach((marker) => {
                if (!source.includes(marker)) {
                    issues.push(`critical regression ${contract.name}: ${rel} is missing ${marker}`);
                }
            });
        });
    });
}

function validateHostRegressionRegistration(issues, summary) {
    const packageJson = JSON.parse(readText(PACKAGE_FILE));
    const scripts = packageJson.scripts || {};
    const expandNpmScript = (name, seen = new Set()) => {
        if (seen.has(name)) return '';
        seen.add(name);
        const command = scripts[name] || '';
        const nested = [];
        const pattern = /\bnpm\s+run\s+([A-Za-z0-9:_-]+)/g;
        let match;
        while ((match = pattern.exec(command)) !== null) nested.push(expandNpmScript(match[1], seen));
        return `${command} ${nested.join(' ')}`;
    };
    const staticChecksManifest = require('./static-checks.manifest.js');
    const expandManifestCommand = (command, seen = new Set()) => {
        const nested = [];
        const pattern = /\bnpm\s+run\s+([A-Za-z0-9:_-]+)/g;
        let match;
        while ((match = pattern.exec(command)) !== null) nested.push(expandNpmScript(match[1], seen));
        return `${command} ${nested.join(' ')}`;
    };
    const staticCommands = staticChecksManifest.map((entry) => expandManifestCommand(entry.command)).join(' ');
    const hostCommands = `${expandNpmScript('test')} ${staticCommands}`;
    const regressionCommands = expandNpmScript('test:regression');

    const validators = fs.readdirSync(path.join(ROOT_DIR, 'scripts'))
        .filter((name) => /^validate-.*\.js$/.test(name))
        .sort();
    const wrappedValidators = new Map([
        ['validate-fim-manifest.js', 'scripts/test-fim-manifest-validator.js']
    ]);
    validators.forEach((name) => {
        const wrapper = wrappedValidators.get(name);
        if (!staticCommands.includes(`scripts/${name}`) && !(wrapper && staticCommands.includes(wrapper))) {
            issues.push(`scripts/static-checks.manifest.js: test:static does not register scripts/${name}`);
        }
    });

    const explicitTestScripts = new Map([
        ['test-build-artifacts.js', 'test:artifacts'],
        ['test-device-health.js', 'test:health']
    ]);
    const nodeTests = fs.readdirSync(path.join(ROOT_DIR, 'scripts'))
        .filter((name) => /^test-.*\.js$/.test(name))
        .sort();
    nodeTests.forEach((name) => {
        const explicitScript = explicitTestScripts.get(name);
        if (explicitScript) {
            if (!(scripts[explicitScript] || '').includes(`scripts/${name}`)) {
                issues.push(`package.json: ${explicitScript} does not register scripts/${name}`);
            }
        } else if (!hostCommands.includes(`scripts/${name}`)) {
            issues.push(`package.json: default host regression does not register scripts/${name}`);
        }
    });

    if (!(scripts.test || '').includes('test:host')) issues.push('package.json: npm test must delegate to test:host');
    ['test:static', 'test:contracts', 'test:unit'].forEach((name) => {
        if (!hostCommands.includes(scripts[name] || '__missing__')) issues.push(`package.json: test:host does not include ${name}`);
    });
    if (!regressionCommands.includes('scripts/run-native-tests.js') ||
        !regressionCommands.includes(scripts['test:host'] || '__missing__')) {
        issues.push('package.json: test:regression must run both host and native regressions');
    }
    if (!(scripts['test:firmware'] || '').includes('scripts/run-firmware-tests.js')) {
        issues.push('package.json: test:firmware must use scripts/run-firmware-tests.js');
    }
    if ((scripts['test:e2e:collect'] || '') !== 'npm --prefix test/browser run test -- --list') {
        issues.push('package.json: test:e2e:collect must parse the isolated browser test project');
    }
    if (!(scripts['test:unit'] || '').includes('scripts/run-unit-tests.js')) {
        issues.push('package.json: test:unit must use scripts/run-unit-tests.js');
    }
    const unitRunner = readText(UNIT_RUNNER_FILE);
    if (!unitRunner.includes("'unittest', 'discover'") || !unitRunner.includes("'-s', 'test/unit'") ||
        !unitRunner.includes("'-p', 'test_*.py'")) {
        issues.push('scripts/run-unit-tests.js: Python tests must discover test/unit/test_*.py');
    }
    const ciText = readText(CI_FILE);
    if (!/^\s*run:\s*npm run test:regression\s*$/m.test(ciText)) {
        issues.push('.github/workflows/ci.yml: regression job must run npm run test:regression');
    }
    if (!/^\s*run:\s*npm run test:e2e:collect\s*$/m.test(ciText) ||
        !/^\s*run:\s*npm ci --prefix test\/browser\s*$/m.test(ciText)) {
        issues.push('.github/workflows/ci.yml: browser test collection must install its isolated dependencies and parse every spec');
    }
    const testAllText = readText(TEST_ALL_FILE);
    if (!testAllText.includes('Invoke-External @("npm", "test")')) {
        issues.push('scripts/test-all.ps1: static gate must reuse npm test');
    }
    if (!/\[string\]\$BaseUrl\s*=\s*""/.test(testAllText) ||
        !testAllText.includes('refuses the device AP address 192.168.4.1')) {
        issues.push('scripts/test-all.ps1: live-device checks must require an explicit non-AP STA URL');
    }
    summary.hostScripts = validators.length + nodeTests.length + 1;
}

function main() {
    const issues = [];
    if (!fs.existsSync(MAIN_FILE)) throw new Error(`Missing ${relative(MAIN_FILE)}`);

    const mainSource = readText(MAIN_FILE);
    const mainCode = maskCppNonCode(mainSource);
    const mainDeclList = findMatches(mainCode, /^\s*extern\s+void\s+(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm);
    const mainCallList = findMatches(mainCode, /^\s*(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm);
    const mainDecls = new Set(mainDeclList);
    const mainCalls = new Set(mainCallList);
    const summary = { files: 0, groups: 0, tests: 0, hostScripts: 0 };
    const globalGroups = new Map();
    const files = listTestFiles(TEST_DIR, issues);

    files.forEach((filePath) => {
        const source = readText(filePath);
        validateTestAnalysis(source, analyzeTestSource(source), relative(filePath), mainDecls, mainCalls,
            issues, summary, globalGroups);
    });

    duplicates(mainDeclList).forEach((name) => issues.push(`test/test_main.cpp: duplicate extern declaration ${name}`));
    duplicates(mainCallList).forEach((name) => issues.push(`test/test_main.cpp: duplicate call ${name}`));
    globalGroups.forEach((locations, name) => {
        if (locations.length > 1) issues.push(`${name} is defined in multiple files: ${locations.join(', ')}`);
    });
    mainDecls.forEach((name) => {
        if (!mainCalls.has(name)) issues.push(`test/test_main.cpp: ${name} is declared but not called`);
    });
    mainCalls.forEach((name) => {
        if (!mainDecls.has(name)) issues.push(`test/test_main.cpp: ${name} is called but has no extern declaration`);
        if (!globalGroups.has(name)) issues.push(`test/test_main.cpp: ${name} is called but no test file defines it`);
    });

    validateFirmwareMatrix(issues);
    validateHostRegressionRegistration(issues, summary);
    validateCriticalRegressionContracts(issues);

    if (issues.length > 0) {
        console.error('FastBee test coverage validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exitCode = 1;
        return false;
    }
    console.log(`FastBee test coverage OK: files=${summary.files}, groups=${summary.groups}, cases=${summary.tests}, hostScripts=${summary.hostScripts}, criticalContracts=${CRITICAL_REGRESSION_CONTRACTS.length}`);
    return true;
}

if (require.main === module) main();

module.exports = {
    analyzeTestSource,
    CRITICAL_REGRESSION_CONTRACTS,
    extractCiFirmwareEnvironments,
    listTestFiles,
    maskCppNonCode,
    validateCriticalRegressionContracts,
    validateSameSet,
    validateTestAnalysis
};
