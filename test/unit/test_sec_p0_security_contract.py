"""Focused source contracts for the SEC-P0 hardening boundary."""

from __future__ import annotations

import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SYSTEM_ROUTE = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(
    encoding="utf-8"
)
PERIPH_EXEC_ROUTE = (
    ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
).read_text(encoding="utf-8")
DEVICE_CONFIG = (ROOT / "web-src/modules/runtime/device-config.js").read_text(
    encoding="utf-8"
)
REQUEST_GOVERNOR = (ROOT / "web-src/js/request-governor.js").read_text(
    encoding="utf-8"
)
DEVICE_PAGE = (ROOT / "web-src/pages/device.html").read_text(encoding="utf-8")

SAFE_TRANSFER_NAMES = {
    "device.json",
    "features.json",
    "peripherals.json",
    "periph_exec.json",
    "rule_scripts.json",
    # Layout/bindings and history source settings contain no credentials.
    # Recorded samples live separately under /data and are not transferred.
    "screen_panel.json",
    "point_history.json",
    "ir_keys.json",
}
SENSITIVE_CONFIG_NAMES = {
    "network.json",
    "protocol.json",
    "auth.json",
    "users.json",
}


def braced_block(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")

    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


def route_block(source: str, path: str, method: str) -> str:
    uri = re.escape(f'"{path}"')
    registration = re.search(
        rf'server->on\(\s*(?:{uri}|AsyncURIMatcher::exact\(\s*{uri}\s*\))\s*,\s*{re.escape(method)}\b',
        source,
    )
    if registration is None:
        raise AssertionError(f"missing {method} route {path}")
    start = registration.start()
    end = source.find("server->on(", registration.end())
    return source[start : end if end >= 0 else len(source)]


def string_array(source: str, name: str) -> set[str]:
    match = re.search(
        rf"static\s+const\s+char\s*\*\s*const\s+{re.escape(name)}\[\]\s*=\s*"
        r"\{(?P<body>.*?)\};",
        source,
        re.S,
    )
    if match is None:
        raise AssertionError(f"missing fixed string array {name}")
    return set(re.findall(r'"([A-Za-z0-9_.-]+\.json)"', match.group("body")))


class SecP0SecurityContractTests(unittest.TestCase):
    def test_periph_exec_run_requires_auth_and_developer_mode_before_execution(self):
        registration = route_block(
            PERIPH_EXEC_ROUTE, "/api/periph-exec/run", "HTTP_POST"
        )
        self.assertIn("handleRunOnce(request)", registration)

        handler = braced_block(
            PERIPH_EXEC_ROUTE,
            "void PeriphExecRouteHandler::handleRunOnce(AsyncWebServerRequest* request)",
        )
        auth_at = handler.find("if (!ctx->requireAuth(request)) return;")
        developer_at = handler.find("if (!ctx->requireDeveloperMode(request)) return;")
        execute_at = handler.find("mgr.runOnce(")
        self.assertGreaterEqual(auth_at, 0)
        self.assertGreaterEqual(developer_at, 0)
        self.assertGreaterEqual(execute_at, 0)
        self.assertLess(auth_at, developer_at)
        self.assertLess(developer_at, execute_at)

    def test_factory_reset_requires_auth_confirmation_and_password_before_mutation(self):
        route = route_block(
            SYSTEM_ROUTE, "/api/system/factory-reset", "HTTP_POST"
        )
        auth_at = route.find("if (!ctx->requireAuth(request)) return;")
        confirmation_at = route.find(
            'ctx->getParamValue(request, "confirm", "")'
        )
        reset_guard_at = route.find('confirmation != "RESET"')
        password_at = route.find(
            'ctx->getParamValue(request, "password", "")'
        )
        verify_at = route.find(
            "ctx->verifyCurrentUserPassword(request, password)"
        )
        mutate_at = route.find("writeDefault(")
        for position in (
            auth_at,
            confirmation_at,
            reset_guard_at,
            password_at,
            verify_at,
            mutate_at,
        ):
            self.assertGreaterEqual(position, 0)
        self.assertEqual(
            sorted(
                (
                    auth_at,
                    confirmation_at,
                    reset_guard_at,
                    password_at,
                    verify_at,
                    mutate_at,
                )
            ),
            [
                auth_at,
                confirmation_at,
                reset_guard_at,
                password_at,
                verify_at,
                mutate_at,
            ],
        )
        self.assertIn(
            'ctx->sendError(request, 400, "Factory reset requires explicit RESET confirmation")',
            route,
        )
        self.assertIn(
            'ctx->sendError(request, 403, "Current password verification failed")',
            route,
        )

    def test_factory_defaults_disable_developer_mode(self):
        factory_route = route_block(
            SYSTEM_ROUTE, "/api/system/factory-reset", "HTTP_POST"
        )
        self.assertIn(r'\"developerModeEnabled\":false', factory_route)
        device_defaults = json.loads(
            (ROOT / "data/config/device.json").read_text(encoding="utf-8")
        )
        self.assertIs(device_defaults.get("developerModeEnabled"), False)

    def test_transfer_endpoints_share_a_fixed_non_sensitive_allowlist(self):
        export_names = string_array(SYSTEM_ROUTE, "kAllowedTransferConfigFiles")
        import_names = string_array(SYSTEM_ROUTE, "kAllowedImportConfigFiles")
        self.assertEqual(export_names, SAFE_TRANSFER_NAMES)
        self.assertEqual(import_names, SAFE_TRANSFER_NAMES)
        self.assertTrue(export_names.isdisjoint(SENSITIVE_CONFIG_NAMES))
        self.assertTrue(import_names.isdisjoint(SENSITIVE_CONFIG_NAMES))

        list_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/list", "HTTP_GET"
        )
        export_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/export", "HTTP_GET"
        )
        inline_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/import", "HTTP_POST"
        )
        chunk_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/import-chunk", "HTTP_POST"
        )

        guarded_operations = (
            (
                list_route,
                "isAllowedConfigTransferFileName(name)",
                "files.add<JsonObject>()",
            ),
            (
                export_route,
                "!isAllowedConfigTransferFileName(name)",
                "configPathForName(name)",
            ),
            (
                inline_route,
                "!isAllowedConfigImportFileName(name)",
                "configPathForName(name)",
            ),
            (
                chunk_route,
                "!isAllowedConfigImportFileName(name)",
                "configPathForName(name)",
            ),
        )
        for route, guard, operation in guarded_operations:
            guard_at = route.find(guard)
            operation_at = route.find(operation)
            self.assertGreaterEqual(guard_at, 0)
            self.assertGreaterEqual(operation_at, 0)
            self.assertLess(guard_at, operation_at)
        for route in (list_route, export_route, inline_route, chunk_route):
            self.assertIn("if (!ctx->requireAuth(request)) return;", route)

    def test_cleanup_preserves_sensitive_runtime_configuration(self):
        known = braced_block(
            SYSTEM_ROUTE, "bool isKnownRuntimeConfigFileName(const String& name)"
        )
        for name in SENSITIVE_CONFIG_NAMES:
            with self.subTest(name=name):
                self.assertIn(f'name == "{name}"', known)

        suffix_match = re.search(
            r"generationSuffixes\[\]\s*=\s*\{(?P<body>.*?)\};", known, re.S
        )
        self.assertIsNotNone(suffix_match)
        generation_suffixes = re.findall(r'"([.a-z]+)"', suffix_match.group("body"))
        self.assertEqual(
            set(generation_suffixes),
            {".bak", ".tmp", ".factory.tmp", ".import.tmp"},
        )
        # The helper returns on the first matching suffix, so specific suffixes
        # must be checked before the generic .tmp suffix.
        self.assertLess(
            generation_suffixes.index(".factory.tmp"),
            generation_suffixes.index(".tmp"),
        )
        self.assertLess(
            generation_suffixes.index(".import.tmp"),
            generation_suffixes.index(".tmp"),
        )
        self.assertIn("return isKnownRuntimeConfigFileName(", known)

        cleanup = braced_block(SYSTEM_ROUTE, "int cleanupStaleConfigFiles()")
        guard_at = cleanup.find("if (!isKnownRuntimeConfigFileName(name))")
        remove_at = cleanup.find("LittleFS.remove(path)")
        self.assertGreaterEqual(guard_at, 0)
        self.assertGreaterEqual(remove_at, 0)
        self.assertLess(guard_at, remove_at)

    def test_file_content_and_save_require_auth_and_developer_mode(self):
        routes = (
            route_block(SYSTEM_ROUTE, "/api/files/content", "HTTP_GET"),
            route_block(SYSTEM_ROUTE, "/api/files/save", "HTTP_POST"),
        )
        for route in routes:
            auth_at = route.find("if (!ctx->requireAuth(request)) return;")
            developer_at = route.find(
                "if (!ctx->requireDeveloperMode(request)) return;"
            )
            path_at = route.find('request->hasParam("path"')
            self.assertGreaterEqual(auth_at, 0)
            self.assertGreaterEqual(developer_at, 0)
            self.assertGreaterEqual(path_at, 0)
            self.assertLess(auth_at, developer_at)
            self.assertLess(developer_at, path_at)

    def test_file_content_limits_config_reads_to_transfer_allowlist(self):
        route = route_block(SYSTEM_ROUTE, "/api/files/content", "HTTP_GET")
        config_guard_at = route.find('if (path.startsWith("/config/"))')
        allowlist_at = route.find("!isAllowedConfigTransferFileName(name)")
        exists_at = route.find("LittleFS.exists(path)")
        open_at = route.find('LittleFS.open(path, "r")')
        for position in (config_guard_at, allowlist_at, exists_at, open_at):
            self.assertGreaterEqual(position, 0)
        self.assertLess(config_guard_at, allowlist_at)
        self.assertLess(allowlist_at, exists_at)
        self.assertLess(allowlist_at, open_at)

        rejected = braced_block(
            route, "if (!isAllowedConfigTransferFileName(name))"
        )
        self.assertIn("ctx->sendError(request, 403", rejected)
        self.assertRegex(rejected, r"\breturn\s*;")
        self.assertEqual(
            string_array(SYSTEM_ROUTE, "kAllowedTransferConfigFiles"),
            SAFE_TRANSFER_NAMES,
        )

    def test_file_save_blocks_every_direct_config_write(self):
        route = route_block(SYSTEM_ROUTE, "/api/files/save", "HTTP_POST")
        config_guard_at = route.find('if (path.startsWith("/config/"))')
        extension_at = route.find('path.endsWith(".json")')
        open_at = route.find('LittleFS.open(path, "w")')
        for position in (config_guard_at, extension_at, open_at):
            self.assertGreaterEqual(position, 0)
        self.assertLess(config_guard_at, extension_at)
        self.assertLess(config_guard_at, open_at)

        rejected = braced_block(route, 'if (path.startsWith("/config/"))')
        self.assertIn("ctx->sendError(request, 409", rejected)
        self.assertRegex(rejected, r"\breturn\s*;")

    def test_file_api_rejects_ambiguous_paths_before_filesystem_io(self):
        path_guard = braced_block(
            SYSTEM_ROUTE, "bool isSafeFileApiPath(const String& path)"
        )
        for rejected_component in (
            'path.indexOf("..") < 0',
            "path.indexOf('\\\\') < 0",
            'path.indexOf("//") < 0',
            'path.indexOf("/./") < 0',
        ):
            with self.subTest(component=rejected_component):
                self.assertIn(rejected_component, path_guard)

        routes_and_io = (
            (
                route_block(SYSTEM_ROUTE, "/api/files/content", "HTTP_GET"),
                'LittleFS.open(path, "r")',
            ),
            (
                route_block(SYSTEM_ROUTE, "/api/files/save", "HTTP_POST"),
                'LittleFS.open(path, "w")',
            ),
        )
        for route, filesystem_io in routes_and_io:
            guard_at = route.find("if (!isSafeFileApiPath(path))")
            io_at = route.find(filesystem_io)
            self.assertGreaterEqual(guard_at, 0)
            self.assertGreaterEqual(io_at, 0)
            self.assertLess(guard_at, io_at)

    def test_imports_validate_then_atomically_commit_without_overwriting_active_config(self):
        inline_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/import", "HTTP_POST"
        )
        inline_parse_at = inline_route.find(
            "deserializeJson(importedConfig, content.c_str(), content.length())"
        )
        inline_commit_at = inline_route.find(
            "configStorage.saveImportedJSONConfig(path, importedConfig)"
        )
        self.assertGreaterEqual(inline_parse_at, 0)
        self.assertGreaterEqual(inline_commit_at, 0)
        self.assertLess(inline_parse_at, inline_commit_at)
        self.assertNotIn('LittleFS.open(path, "w")', inline_route)
        self.assertNotIn("LittleFS.remove(path)", inline_route)

        chunk_route = route_block(
            SYSTEM_ROUTE, "/api/config/transfer/import-chunk", "HTTP_POST"
        )
        staged_path_at = chunk_route.find('String stagedPath = path + ".import.tmp"')
        staged_write_at = chunk_route.find(
            'LittleFS.open(stagedPath, "a")'
        )
        final_parse_at = chunk_route.find(
            "deserializeJson(importedConfig, staged)"
        )
        final_commit_at = chunk_route.find(
            "configStorage.saveImportedJSONConfig(path, importedConfig)"
        )
        committed_at = chunk_route.find(
            "ordinaryImport.committed()", final_commit_at
        )
        for position in (
            staged_path_at,
            staged_write_at,
            final_parse_at,
            final_commit_at,
            committed_at,
        ):
            self.assertGreaterEqual(position, 0)
        self.assertLess(staged_path_at, staged_write_at)
        self.assertLess(staged_write_at, final_parse_at)
        self.assertLess(final_parse_at, final_commit_at)
        self.assertLess(final_commit_at, committed_at)
        self.assertNotIn("LittleFS.remove(stagedPath)", chunk_route)
        self.assertNotIn("cleanupStaleConfigFiles()", chunk_route)
        self.assertIn("if (!replay && index == total - 1)", chunk_route)
        self.assertNotIn('LittleFS.open(path, index == 0 ? "w" : "a")', chunk_route)
        self.assertNotIn("LittleFS.remove(path)", chunk_route)

    def test_factory_reset_frontend_submits_confirmation_and_password(self):
        factory_reset = braced_block(DEVICE_CONFIG, "\n        factoryReset()")
        self.assertIn("document.getElementById('dev-factory-confirm')", factory_reset)
        self.assertIn("document.getElementById('dev-factory-password')", factory_reset)
        self.assertIn("if (confirmValue !== 'RESET')", factory_reset)
        self.assertIn("if (!password)", factory_reset)
        self.assertRegex(
            factory_reset,
            r"apiFactoryReset\s*\(\s*\{\s*confirm:\s*'RESET'\s*,\s*"
            r"password:\s*password\s*\}\s*\)",
        )
        self.assertIn('id="dev-factory-confirm"', DEVICE_PAGE)
        self.assertIn('id="dev-factory-password"', DEVICE_PAGE)

        transport = braced_block(
            REQUEST_GOVERNOR, "window.apiFactoryReset = function (data)"
        )
        self.assertIn("request('POST', '/api/system/factory-reset'", transport)
        self.assertIn("body: toUrlEncoded(data || {})", transport)


if __name__ == "__main__":
    unittest.main()
