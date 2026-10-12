"""Explicit same-environment inputs and pinned old-library extraction support."""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
from test.unit.test_libdeps_patch_contract import PATCH_MODULE

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_ENVIRONMENT = "esp32c3-F4R0"
WEB_MEMBERS = (
    "library.json", "src/ESPAsyncWebServer.h", "src/WebResponseImpl.h",
    "src/WebResponses.cpp", "src/WebRequest.cpp",
)
OLD_FIXTURE_SHA256 = {
    "library.json": "1654ae4c24b82d8833323647e760a7faca61600ecd6533afd980821bd6e01168",
    "src/ESPAsyncWebServer.h": "cce1722206e166a2693f66d5f087968df6b51c2308a1aeb61710e8969e9db81b",
    "src/WebResponseImpl.h": "6e47bda4b501cb8e9efd93cf2aea4c575f1467e093e92182a3d35abc8bdb291c",
    "src/WebResponses.cpp": "7e2f152ebdcefd617b5c78f80ef6213dab141deb91cf61cb4bc72de6bf398542",
    "src/WebRequest.cpp": "9ac3bb39c0942a0c3d1b2674ab8e9d4af39678275a789a7d49ee5f28501be1a2",
}


@dataclass(frozen=True)
class RuntimeInputs:
    environment: str
    root: Path
    web: Path
    tcp_source: Path
    manifest: dict


def select_runtime_inputs(root: Path, env_name: str, *, report=True) -> RuntimeInputs:
    """Fail explicitly on missing/mismatched inputs; never select independent globs."""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", env_name):
        raise AssertionError("expected one explicit libdeps environment")
    env_dir = root / ".pio/libdeps" / env_name
    web = env_dir / "ESPAsyncWebServer"
    tcp = env_dir / "AsyncTCP"
    manifest = {}
    for directory, name, version, members in (
        (web, "ESPAsyncWebServer", "3.12.0", WEB_MEMBERS),
        (tcp, "AsyncTCP", "3.5.0", ("library.json", "src/AsyncTCP.cpp")),
    ):
        metadata = json.loads((directory / "library.json").read_text(encoding="utf-8"))
        if metadata.get("name") != name or metadata.get("version") != version:
            raise AssertionError(f"{env_name}: expected {name} {version}")
        for member in members:
            raw = (directory / member).read_bytes()
            manifest[f"{env_name}/{name}/{member}"] = {
                "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
            }
    if report:
        print("finite-progress runtime inputs: " + json.dumps(manifest, sort_keys=True))
    return RuntimeInputs(env_name, root, web, tcp / "src/AsyncTCP.cpp", manifest)


def verify_runtime_inputs_unchanged(inputs):
    current = select_runtime_inputs(inputs.root, inputs.environment, report=False)
    if current.manifest != inputs.manifest:
        raise AssertionError("finite-progress runtime inputs changed during extraction/test")


def verify_old_fixture():
    fixture = ROOT / "test/fixtures/espasyncwebserver-3.12.0-pre-finite"
    license_digest = hashlib.sha256((fixture / "LICENSE").read_bytes()).hexdigest()
    if license_digest != "a5681bf9b05db14d86776930017c647ad9e6e56ff6bbcfdf21e5848288dfaf1b":
        raise AssertionError("pinned pre-finite fixture LICENSE SHA256 mismatch")
    for member, expected in OLD_FIXTURE_SHA256.items():
        raw = (fixture / member).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise AssertionError(f"pinned pre-finite fixture SHA256 mismatch: {member}")
        if member != "library.json" and b"_fastbeeCheckProgress" in raw:
            raise AssertionError(f"pinned fixture unexpectedly upgraded: {member}")
    metadata = json.loads((fixture / "library.json").read_text(encoding="utf-8"))
    if metadata.get("name") != "ESPAsyncWebServer" or metadata.get("version") != "3.12.0":
        raise AssertionError("expected pinned ESPAsyncWebServer 3.12.0 fixture")
    return fixture


def copy_web_members(source: Path, destination: Path):
    """Copy exactly the metadata and four source members into a new child directory."""
    destination.mkdir()
    (destination / "src").mkdir()
    for member in WEB_MEMBERS:
        shutil.copyfile(source / member, destination / member)


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


def method(source, signature):
    return signature + " {" + braced_block(source, signature) + "}\n"


def progress_methods(source):
    basic = ""
    if "bool AsyncBasicResponse::_fastbeeCheckProgress(" in source:
        basic = method(source, "bool AsyncBasicResponse::_fastbeeCheckProgress(AsyncWebServerRequest *request)")
    return basic + method(source, "bool AsyncAbstractResponse::_fastbeeCheckProgress(AsyncWebServerRequest *request)") + method(
        source, "void AsyncAbstractResponse::_respond(AsyncWebServerRequest *request)")


def write_progress_headers(temporary, web_library):
    """Compile actual upgraded declarations and inline hooks, not hand-coded substitutes."""
    header = (web_library / "src/ESPAsyncWebServer.h").read_text(encoding="utf-8")
    header, _ = PATCH_MODULE._patch_finite_progress_base(header)
    target = temporary / "WebResponseImpl.h"
    shutil.copyfile(web_library / "src/WebResponseImpl.h", target)
    PATCH_MODULE._patch_web_response_impl(target)
    impl = target.read_text(encoding="utf-8")
    basic, abstract = impl.split("class AsyncAbstractResponse : public AsyncWebServerResponse {", 1)
    basic_fields = ""
    if "_fastbeeProgressInitialized" in basic:
        start = basic.index("  // FastBee patch: bounded finite-response progress")
        end = basic.index("  size_t _fastbeeDrainedHighWater{0};\n", start) + len("  size_t _fastbeeDrainedHighWater{0};\n")
        declaration = "  bool _fastbeeCheckProgress(AsyncWebServerRequest *request) final;\n"
        assert basic.count(declaration) == 1
        basic_fields = basic[start:end] + declaration
    (temporary / "production_basic_progress_fields.inc").write_text(basic_fields, encoding="utf-8")
    signature = "  size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t time) final"
    (temporary / "production_basic_ack.inc").write_text(method(basic, signature), encoding="utf-8")
    (temporary / "production_abstract_ack.inc").write_text(method(abstract, signature), encoding="utf-8")
    base_signature = "  virtual bool _fastbeeCheckProgress(AsyncWebServerRequest *request)"
    (temporary / "production_progress_base.inc").write_text(method(header, base_signature), encoding="utf-8")
    start = abstract.index("  // FastBee patch: bounded finite-response progress")
    last = "  size_t _fastbeeDrainedHighWater{0};\n"
    end = abstract.index(last, start) + len(last)
    fields = abstract[start:end]
    declaration = "  bool _fastbeeCheckProgress(AsyncWebServerRequest *request) final;\n"
    assert abstract.count(declaration) == 1
    (temporary / "production_progress_fields.inc").write_text(fields + declaration, encoding="utf-8")
