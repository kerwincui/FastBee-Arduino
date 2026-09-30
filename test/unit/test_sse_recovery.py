"""Exercise production ACK handling and bounded SSE payload splitting."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class SseRecoveryTests(unittest.TestCase):
    def compile_run(self, source, stdin=''):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='fastbee-sse-recovery-') as temp:
            path = Path(temp) / 'test.cpp'
            path.write_text(source, encoding='utf-8')
            exe = Path(temp) / ('test.exe' if os.name == 'nt' else 'test')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(ROOT / 'include'), str(path), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], input=stdin, capture_output=True,
                                 text=True, encoding='utf-8', timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            return ran.stdout

    def test_chunks_preserve_all_nested_and_escaped_records(self):
        rows = [{'id': 'soil_' + str(i), 'label': '土壤 } [, "quoted" \\ sensor',
                 'value': i, 'quality': 'valid', 'nested': {'list': [1, 2, {'x': 3}]}}
                for i in range(20)]
        source = r'''
#include "network/SseJsonChunks.h"
#include <iostream>
#include <iterator>
#include <string>
#include <cassert>
int main() {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    auto result = FastBee::sendSseObjectArray<512>(input.data(), input.size(), [](const char* part) {
        assert(std::strlen(part) <= 512);
        std::cout << part << std::endl;
        return true;
    });
    assert(result == FastBee::SseChunkResult::Ok);
}
'''
        output = self.compile_run(source, json.dumps(rows, ensure_ascii=False))
        chunks = [json.loads(line) for line in output.splitlines()]
        self.assertGreater(len(chunks), 1)
        self.assertEqual([row for chunk in chunks for row in chunk], rows)

    def test_oversize_invalid_and_backpressure_are_explicit(self):
        self.compile_run(r'''
#include "network/SseJsonChunks.h"
#include <string>
#include <cassert>
int main() {
    unsigned sent=0;
    auto emit=[&](const char*) { ++sent; return true; };
    std::string big="[{\"id\":\"ok\"},{\"label\":\"" + std::string(600,'x') + "\"}]";
    assert(FastBee::sendSseObjectArray<512>(big.data(),big.size(),emit)==FastBee::SseChunkResult::RecordTooLarge);
    assert(sent==0);
    for (const std::string bad : {"[{]", "[{}", "[{},]", "[{}]garbage", "[{\"x\":\"unterminated}]"}) {
        assert(FastBee::sendSseObjectArray<512>(bad.data(),bad.size(),emit)==FastBee::SseChunkResult::InvalidArray);
        assert(sent==0);
    }
    const std::string valid="[{\"id\":1}]";
    assert(FastBee::sendSseObjectArray<512>(valid.data(),valid.size(),[](const char*) { return false; })==FastBee::SseChunkResult::SendFailed);
}
''')

    def test_positive_ack_keeps_healthy_peer_and_preserves_library_bookkeeping(self):
        source = (ROOT / 'src/network/handlers/SSERouteHandler.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(source, signature) for signature in (
            'void SSERouteHandler::trackClient(', 'void SSERouteHandler::noteClientAck(',
            'void SSERouteHandler::cleanupStaleConnections('))
        self.compile_run(r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#define LOG_INFOF(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
static unsigned long clockMs=0;
unsigned long millis(){return clockMs;}
struct AsyncClient {
    std::function<void(void*,AsyncClient*,size_t,uint32_t)> callback;
    void onAck(decltype(callback) cb,void*) {callback=cb;}
};
struct AsyncEventSourceClient {
    AsyncClient tcp;bool live=true;size_t acked=0;
    AsyncClient* client(){return &tcp;}
    bool connected(){return live;}
    void close(){live=false;}
    void _onAck(size_t n,uint32_t){acked+=n;}
};
struct SSERouteHandler {
    static constexpr size_t MAX_SSE_CLIENTS=2;
    static constexpr unsigned long CLIENT_TIMEOUT_MS=120000;
    struct ClientSlot {AsyncEventSourceClient* client=nullptr;unsigned long connectTime=0,lastActiveMs=0;};
    ClientSlot _slots[2];
    struct Stats {unsigned disconnectedCleanups=0,lastCleanupAtMs=0,timedOutClients=0,
        evictedOldestClients=0,lastForcedCloseAtMs=0;} _stats;
    void trackClient(AsyncEventSourceClient*,unsigned long);
    void noteClientAck(AsyncEventSourceClient*,size_t);
    void cleanupStaleConnections(unsigned long);
};
PRODUCTION
int main(){
    SSERouteHandler handler;AsyncEventSourceClient healthy,blocked;
    handler.trackClient(&healthy,0);handler.trackClient(&blocked,0);
    for(unsigned long t=30000;t<=300000;t+=30000){
        clockMs=t;healthy.tcp.callback(nullptr,&healthy.tcp,20,1);
        blocked.tcp.callback(nullptr,&blocked.tcp,0,1);
        handler.cleanupStaleConnections(t);
        assert(healthy.live);
    }
    assert(!blocked.live && healthy.acked==200 && handler._stats.timedOutClients==1);
    clockMs=420001;handler.cleanupStaleConnections(clockMs);
    assert(!healthy.live && handler._stats.timedOutClients==2);
}
'''.replace('PRODUCTION', methods))

if __name__ == '__main__':
    unittest.main()
