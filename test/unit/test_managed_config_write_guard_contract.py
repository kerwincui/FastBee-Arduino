"""Source contracts for blocking raw writes to manager-owned config files."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SYSTEM_ROUTE = (
    ROOT / "src/network/handlers/SystemRouteHandler.cpp"
).read_text(encoding="utf-8")

MANAGED_NAMES = (
    "peripherals.json",
    "periph_exec.json",
    "rule_scripts.json",
)


def function_body(source, signature):
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError("missing C++ function matching {!r}".format(signature))
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError("unterminated C++ function matching {!r}".format(signature))


def route_block(path):
    marker = 'server->on("{}", HTTP_POST'.format(path)
    start = SYSTEM_ROUTE.find(marker)
    if start < 0:
        raise AssertionError("missing POST route {}".format(path))
    end = SYSTEM_ROUTE.find("server->on(", start + len(marker))
    if end < 0:
        end = len(SYSTEM_ROUTE)
    return SYSTEM_ROUTE[start:end]


def assert_rejects_with_409(test_case, route, helper, argument):
    guard = re.search(
        r"if\s*\(\s*{}\s*\(\s*{}\s*\)\s*\)\s*"
        r"\{{(?P<body>.*?)\}}".format(re.escape(helper), re.escape(argument)),
        route,
        re.S,
    )
    test_case.assertIsNotNone(guard, "{} guard is missing".format(helper))
    body = guard.group("body")
    test_case.assertRegex(body, r"sendError\s*\(\s*request\s*,\s*409\s*,")
    test_case.assertRegex(body, r"\breturn\s*;")
    return guard.start()


class ManagedConfigWriteGuardContractTests(unittest.TestCase):
    def test_managed_file_name_guard_covers_every_manager_owned_config(self):
        body = function_body(SYSTEM_ROUTE, r"\bisManagedRuntimeConfigFileName\s*\(")
        for name in MANAGED_NAMES:
            with self.subTest(name=name):
                self.assertIn('name == "{}"'.format(name), body)
        self.assertEqual(
            len(re.findall(r'name\s*==\s*"(?:peripherals|periph_exec|rule_scripts)\.json"', body)),
            len(MANAGED_NAMES),
        )

    def test_managed_path_guard_covers_every_manager_owned_config(self):
        body = function_body(SYSTEM_ROUTE, r"\bisManagedRuntimeConfigPath\s*\(")
        for name in MANAGED_NAMES:
            with self.subTest(name=name):
                self.assertIn('path == "/config/{}"'.format(name), body)
        self.assertEqual(
            len(re.findall(
                r'path\s*==\s*"/config/(?:peripherals|periph_exec|rule_scripts)\.json"',
                body,
            )),
            len(MANAGED_NAMES),
        )

    def test_inline_import_rejects_before_constructing_or_writing_path(self):
        route = route_block("/api/config/transfer/import")
        guard_pos = assert_rejects_with_409(
            self, route, "isManagedRuntimeConfigFileName", "name"
        )
        self.assertLess(guard_pos, route.find("configPathForName(name)"))
        self.assertLess(guard_pos, route.find("configStorage.saveJSONConfig(path, importedConfig)"))
        self.assertIn('request->hasParam("content", true)', route)

    def test_chunk_import_rejects_before_create_append_or_resume(self):
        route = route_block("/api/config/transfer/import-chunk")
        guard_pos = assert_rejects_with_409(
            self, route, "isManagedRuntimeConfigFileName", "name"
        )
        self.assertLess(guard_pos, route.find("configPathForName(name)"))
        self.assertLess(guard_pos, route.find("if (index > 0"))
        self.assertLess(
            guard_pos,
            route.find('LittleFS.open(stagedPath, index == 0 ? "w" : "a")'),
        )
        for parameter in ("chunk", "index", "total"):
            self.assertIn('request->hasParam("{}", true)'.format(parameter), route)

    def test_file_editor_save_rejects_before_extension_check_and_open(self):
        route = route_block("/api/files/save")
        normalize_pos = route.find('if (!path.startsWith("/")) path = "/" + path')
        guard_pos = assert_rejects_with_409(
            self, route, "isManagedRuntimeConfigPath", "path"
        )
        extension_pos = route.find('path.endsWith(".json")')
        open_pos = route.find('LittleFS.open(path, "w")')
        self.assertGreaterEqual(normalize_pos, 0)
        self.assertGreaterEqual(extension_pos, 0)
        self.assertGreaterEqual(open_pos, 0)
        self.assertLess(normalize_pos, guard_pos)
        self.assertLess(guard_pos, extension_pos)
        self.assertLess(guard_pos, open_pos)

    def test_all_three_write_routes_are_registered_as_post(self):
        for path in (
            "/api/config/transfer/import",
            "/api/config/transfer/import-chunk",
            "/api/files/save",
        ):
            with self.subTest(path=path):
                self.assertIn('server->on("{}", HTTP_POST'.format(path), SYSTEM_ROUTE)


if __name__ == "__main__":
    unittest.main()
