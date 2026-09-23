"""Execute the production gzip validator/response path with injected file faults.

The file and HTTP transport are stubs; CRC, rewind decisions, 304 selection,
and cache/encoding headers are extracted from WebHandlerContext.cpp.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class StaticGzipCursorRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise unittest.SkipTest("A C++17 compiler is required")
        source = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
        etag_start = source.index("static bool getStaticGzipEtag(")
        etag = source[etag_start:source.index("\n}\n", etag_start) + 3]
        preparation_start = source.index("    char staticEtag[11] = {};")
        preparation = source[preparation_start:source.index(
            "    StaticInFlightLease staticLease", preparation_start)]
        response_start = source.index("    AsyncWebServerResponse *response = new AsyncFileResponse(")
        response = source[response_start:source.index("\n}\n", response_start)]
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-gzip-cursor-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        translation_unit = HARNESS.replace("// PRODUCTION_ETAG", etag).replace(
            "// PRODUCTION_PREPARATION", preparation).replace("// PRODUCTION_RESPONSE", response)
        cpp = directory / "gzip_cursor.cpp"
        cpp.write_text(translation_unit, encoding="utf-8")
        cls.executable = directory / ("gzip_cursor.exe" if os.name == "nt" else "gzip_cursor")
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(cls.executable)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normal_gzip_keeps_complete_body_etag_and_cache_headers(self):
        self.run_case("normal")

    def test_crc_short_read_rewinds_and_sends_complete_body_without_etag(self):
        self.run_case("crc_short")

    def test_initial_trailer_seek_failure_still_sends_complete_body(self):
        self.run_case("trailer_seek")

    def test_rewind_failure_closes_file_and_sends_explicit_error(self):
        self.run_case("rewind_failure")

    def test_successful_seek_with_wrong_position_cannot_send_partial_200(self):
        self.run_case("rewind_wrong_position")

    def test_matching_etag_returns_304_without_a_body(self):
        self.run_case("match")

    def test_crc_short_read_never_reuses_a_matching_etag(self):
        self.run_case("crc_short_match")

    def test_rewind_failure_does_not_return_a_false_304(self):
        self.run_case("rewind_failure_match")

    def test_service_worker_gzip_keeps_no_cache_policy(self):
        self.run_case("service_worker")


HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

struct String : std::string {
    using std::string::string;
    String(unsigned long n) : std::string(std::to_string(n)) {}
};

enum class Fault { None, CrcShort, TrailerSeek, RewindFailure, RewindWrongPosition };
struct File {
    std::string bytes = std::string("asset-gzip-body") + std::string("\x78\x56\x34\x12\x10\0\0\0", 8);
    size_t cursor = 0;
    bool opened = true;
    Fault fault = Fault::None;
    explicit operator bool() const { return opened; }
    size_t size() const { return bytes.size(); }
    size_t position() const { return cursor; }
    bool seek(size_t to) {
        if (to == size() - 8 && fault == Fault::TrailerSeek) return false;
        if (to == 0 && fault == Fault::RewindFailure) return false;
        if (to == 0 && fault == Fault::RewindWrongPosition) return true;
        cursor = to;
        return true;
    }
    size_t read(uint8_t* out, size_t count) {
        if (fault == Fault::CrcShort && cursor == size() - 8) count = 2;
        count = std::min(count, size() - cursor);
        std::memcpy(out, bytes.data() + cursor, count);
        cursor += count;
        return count;
    }
    void close() { opened = false; }
};

struct AsyncWebServerResponse {
    int status;
    size_t contentLength = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    explicit AsyncWebServerResponse(int code = 0) : status(code) {}
    virtual ~AsyncWebServerResponse() = default;
    void addHeader(const std::string& name, const std::string& value) { headers[name] = value; }
};
struct AsyncFileResponse : AsyncWebServerResponse {
    AsyncFileResponse(File file, const String&, const String&, bool)
        : AsyncWebServerResponse(200) {
        contentLength = file.size();
        body = file.bytes.substr(file.position());
    }
};
struct Request {
    std::map<std::string, std::string> headers;
    AsyncWebServerResponse result;
    bool hasHeader(const std::string& key) { return headers.count(key) != 0; }
    std::string header(const std::string& key) { return headers.at(key); }
    AsyncWebServerResponse* beginResponse(int code) { return new AsyncWebServerResponse(code); }
    void send(AsyncWebServerResponse* response) { result = *response; delete response; }
};
void sendStaticBusyResponse(Request* request, const String&) {
    request->send(new AsyncWebServerResponse(503));
}
void registerStaticInFlightRelease(Request*, int) {}

// PRODUCTION_ETAG

bool serve(File& openFile, Request* request, bool isServiceWorker) {
    const bool sentGzippedContent = true;
    const bool isCacheableStatic = !isServiceWorker;
    const unsigned long cacheDuration = 86400;
    const String path = "/www/js/chunk-3-state.js";
    const String contentType = "application/javascript";
    const bool limitInFlight = false;
    const int staticLease = 0;
    // PRODUCTION_PREPARATION
    // PRODUCTION_RESPONSE
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    File file;
    Request request;
    if (name == "crc_short" || name == "crc_short_match") file.fault = Fault::CrcShort;
    if (name == "trailer_seek") file.fault = Fault::TrailerSeek;
    if (name == "rewind_failure" || name == "rewind_failure_match") file.fault = Fault::RewindFailure;
    if (name == "rewind_wrong_position") file.fault = Fault::RewindWrongPosition;
    if (name == "match" || name == "crc_short_match" || name == "rewind_failure_match")
        request.headers["If-None-Match"] = "\"78563412\"";
    assert(serve(file, &request, name == "service_worker"));
    const auto& result = request.result;
    if (file.fault == Fault::RewindFailure || file.fault == Fault::RewindWrongPosition) {
        assert(result.status == 503);
        assert(!file.opened);
        assert(result.body.empty());
        assert(result.headers.count("ETag") == 0);
        return 0;
    }
    assert(result.headers.at("Connection") == "close");
    assert(result.headers.at("Cache-Control") ==
           (name == "service_worker" ? "no-cache" : "public, max-age=86400"));
    if (name == "match") {
        assert(result.status == 304);
        assert(result.body.empty());
        assert(!file.opened);
        assert(result.headers.at("ETag") == "\"78563412\"");
        return 0;
    }
    assert(result.status == 200);
    assert(result.contentLength == file.bytes.size());
    assert(result.body == file.bytes);
    assert(result.headers.at("Content-Encoding") == "gzip");
    if (file.fault == Fault::CrcShort || file.fault == Fault::TrailerSeek)
        assert(result.headers.count("ETag") == 0);
    else
        assert(result.headers.at("ETag") == "\"78563412\"");
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()
