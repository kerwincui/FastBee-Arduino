"""Run production save/cache methods with failure injected at the disk boundary."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(signature)


class ConfigStorageFailedCommitTests(unittest.TestCase):
    def test_production_save_preserves_committed_and_deferred_generations(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("C++17 compiler required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Existing ArduinoJson headers required")
        source = (ROOT / "src/systems/ConfigStorage.cpp").read_text(encoding="utf-8")
        production = "\n".join(method(source, signature) for signature in (
            "bool ConfigStorage::saveJSONConfig(",
            "ConfigStorage::ConfigCacheEntry* ConfigStorage::findInCache(",
            "bool ConfigStorage::updateCacheEntry(",
            "void ConfigStorage::flushDirtyEntries(",
        ))
        harness = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <utility>

struct String : std::string {
    using std::string::string;
    using std::string::operator=;
    int indexOf(const char* value) const {
        auto result=find(value); return result==npos ? -1 : static_cast<int>(result);
    }
    size_t write(uint8_t byte) { push_back(static_cast<char>(byte)); return 1; }
    size_t write(const uint8_t* bytes,size_t count) {
        append(reinterpret_cast<const char*>(bytes),count); return count;
    }
};
unsigned long fakeNow=100;
unsigned long millis() { return fakeNow; }

struct ConfigStorage {
    struct Mutex { bool available=true, held=false; } _cacheMutex;
    struct CacheLock {
        Mutex& mutex; bool locked;
        CacheLock(Mutex& m):mutex(m),locked(m.available && !m.held) {
            if (locked) mutex.held=true;
        }
        ~CacheLock() { if (locked) mutex.held=false; }
        operator bool() const { return locked; }
    };
    struct ConfigCacheEntry {
        String filename, rawJson;
        unsigned long lastLoadTime=0, debounceUntil=0;
        time_t fileModifyTime=0;
        bool dirty=false;
        uint8_t accessCount=0;
    };
    static const size_t MAX_CONFIG_CACHE_ENTRIES=2, CACHE_MAX_FILE_BYTES=4096;
    ConfigCacheEntry _cache[MAX_CONFIG_CACHE_ENTRIES];
    size_t _cacheSize=0;
    bool _initialized=true, diskFailure=false;
    int diskWrites=0, rawWrites=0;
    std::map<std::string,std::string> disk;
    bool initializeInternal() { return false; }
    bool evictLRUEntry() { return false; }
    bool saveJSONConfig(const String&,const JsonDocument&);
    ConfigCacheEntry* findInCache(const String&);
    bool updateCacheEntry(const String&,const JsonDocument&,time_t);
    void flushDirtyEntries(bool force=false);
    bool flushToDisk(const String& path,const JsonDocument& doc) {
        // All cache users must see either the old or committed generation.
        assert(_cacheMutex.held);
        ++diskWrites;
        if (diskFailure) return false;
        std::string raw; serializeJson(doc,raw); disk[path]=raw; return true;
    }
    bool flushRawToDisk(const String& path,const String& raw) {
        assert(_cacheMutex.held);
        ++rawWrites;
        if (diskFailure) return false;
        disk[path]=raw; return true;
    }
    void seed(const String& path,int revision,bool dirty=false) {
        JsonDocument doc; doc["revision"]=revision;
        assert(updateCacheEntry(path,doc,123));
        findInCache(path)->dirty=dirty;
        std::string raw; serializeJson(doc,raw); disk[path]=raw;
    }
    int cachedRevision(const String& path) {
        auto* entry=findInCache(path);
        if (!entry || entry->rawJson.empty()) return -1;
        JsonDocument parsed; assert(!deserializeJson(parsed,entry->rawJson.c_str()));
        return parsed["revision"].as<int>();
    }
    int diskRevision(const String& path) {
        JsonDocument parsed; assert(!deserializeJson(parsed,disk.at(path)));
        return parsed["revision"].as<int>();
    }
};
'''
        checks = r'''
int main() {
    JsonDocument next; next["revision"]=2;
    for (const char* path:{"/config/protocol.json","/config/network.json",
                          "/config/users.json","/config/features.json"}) {
        ConfigStorage storage; storage.seed(path,1);
        storage.diskFailure=true;
        assert(!storage.saveJSONConfig(path,next));
        assert(storage.cachedRevision(path)==1);
        assert(storage.diskRevision(path)==1);
        assert(!storage.findInCache(path)->dirty);
        storage.flushDirtyEntries(true);
        assert(storage.rawWrites==0); // No candidate can leak via a later flush.
        storage.diskFailure=false;
        assert(storage.saveJSONConfig(path,next));
        assert(storage.diskRevision(path)==2);
        assert(storage.cachedRevision(path)==-1); // Reload committed disk state.
    }
    {
        ConfigStorage storage; storage.diskFailure=true;
        assert(!storage.saveJSONConfig("/config/protocol.json",next));
        assert(storage._cacheSize==0); // A first save cannot expose uncommitted data.
        assert(storage.disk.empty());
    }
    {
        ConfigStorage storage; storage.seed("/config/screen_panel.json",1);
        assert(storage.saveJSONConfig("/config/screen_panel.json",next));
        auto* entry=storage.findInCache("/config/screen_panel.json");
        assert(entry->dirty && entry->debounceUntil==fakeNow+3000);
        assert(storage.cachedRevision("/config/screen_panel.json")==2);
        assert(storage.diskWrites==0 && storage.diskRevision("/config/screen_panel.json")==1);
        storage.diskFailure=true;
        storage.flushDirtyEntries(true);
        assert(entry->dirty && storage.cachedRevision("/config/screen_panel.json")==2);
        storage.diskFailure=false;
        storage.flushDirtyEntries(true);
        assert(!entry->dirty && storage.diskRevision("/config/screen_panel.json")==2);
    }
    {
        ConfigStorage storage; storage.seed("/config/screen_panel.json",1);
        assert(storage.saveJSONConfig("/config/screen_panel.json",next));
        JsonDocument large; large["revision"]=3; large["padding"]=std::string(5000,'x');
        storage.diskFailure=true;
        assert(!storage.saveJSONConfig("/config/screen_panel.json",large));
        assert(storage.cachedRevision("/config/screen_panel.json")==2);
        assert(storage.findInCache("/config/screen_panel.json")->dirty);
        assert(storage.diskRevision("/config/screen_panel.json")==1);
        storage.diskFailure=false;
        assert(storage.saveJSONConfig("/config/screen_panel.json",large));
        assert(storage.diskRevision("/config/screen_panel.json")==3);
        assert(storage.cachedRevision("/config/screen_panel.json")==-1);
        assert(!storage.findInCache("/config/screen_panel.json")->dirty);
        storage.flushDirtyEntries(true);
        assert(storage.rawWrites==0); // Stale deferred data cannot overwrite the large commit.
    }
    {
        ConfigStorage storage; storage.seed("/config/protocol.json",1);
        storage._cacheMutex.available=false;
        assert(!storage.saveJSONConfig("/config/protocol.json",next));
        assert(storage.diskWrites==0 && storage.cachedRevision("/config/protocol.json")==1);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="fastbee-config-commit-") as temporary:
            path = Path(temporary)
            cpp = path / "commit.cpp"
            binary = path / ("commit.exe" if os.name == "nt" else "commit")
            cpp.write_text(harness + production + checks, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-DFASTBEE_ENABLE_STORAGE_CACHE=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                "-DARDUINOJSON_ENABLE_STD_STRING=1", f"-I{json_headers[0].parent}",
                str(cpp), "-o", str(binary),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", env=environment, timeout=60)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", env=environment, timeout=15)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
