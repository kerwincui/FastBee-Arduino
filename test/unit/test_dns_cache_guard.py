"""Run the production DNS wrapper against a strict simulated lwIP core."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DnsCacheGuardTests(unittest.TestCase):
    def test_raw_callbacks_require_core_and_preserve_order(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='fastbee-dns-guard-') as directory:
            temp = Path(directory)
            (temp / 'lwip/priv').mkdir(parents=True)
            (temp / 'lwip/priv/tcpip_priv.h').write_text('''#pragma once
using err_t = int;
constexpr int ERR_OK = 0;
struct tcpip_api_call_data { int err; };
err_t tcpip_api_call(err_t (*)(tcpip_api_call_data*), tcpip_api_call_data*);
''')
            (temp / 'lwip/sys.h').write_text('''#pragma once
enum { LWIP_CORE_LOCK_QUERY_HOLDER };
bool sys_thread_tcpip(int);
''')
            (temp / 'esp32-hal-log.h').write_text('#pragma once\nvoid log_w(const char*, int);\n')
            harness = temp / 'main.cpp'
            harness.write_text('''#include <cassert>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/sys.h>
extern "C" void __wrap_dns_clear_cache();
bool owner = false, recurse = false, dispatchFail = false;
int clears = 0, dispatches = 0, warnings = 0;
bool sys_thread_tcpip(int query) { assert(query == LWIP_CORE_LOCK_QUERY_HOLDER); return owner; }
void log_w(const char*, int code) { assert(code == -1); ++warnings; }
extern "C" void __real_dns_clear_cache() {
  assert(owner); ++clears;
  if (recurse) { recurse = false; __wrap_dns_clear_cache(); assert(owner); }
}
err_t tcpip_api_call(err_t (*fn)(tcpip_api_call_data*), tcpip_api_call_data* arg) {
  assert(!owner); ++dispatches;
  if (dispatchFail) return -1;
  owner = true; auto result = fn(arg); assert(owner); owner = false; return result;
}
int main() {
  __wrap_dns_clear_cache(); assert(clears == 1 && dispatches == 1 && !owner);
  owner = true; __wrap_dns_clear_cache(); assert(clears == 2 && dispatches == 1 && owner); owner = false;
  recurse = true; __wrap_dns_clear_cache(); assert(clears == 4 && dispatches == 2 && !owner);
  dispatchFail = true; __wrap_dns_clear_cache(); assert(clears == 4 && dispatches == 3 && warnings == 1 && !owner);
  dispatchFail = false; __wrap_dns_clear_cache(); assert(clears == 5 && dispatches == 4 && !owner);
}
''')
            exe = temp / ('guard.exe' if os.name == 'nt' else 'guard')
            compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINO_ARCH_ESP32=1', f'-I{temp}', str(harness),
                str(ROOT / 'src/network/DnsCacheGuard.cpp'), '-o', str(exe)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_embedded_linker_enables_wrapper(self):
        config = (ROOT / 'platformio.ini').read_text(encoding='utf-8')
        base = config.split('[esp32_base]', 1)[1].split('[all_src_filter]', 1)[0]
        self.assertIn('-Wl,--wrap=dns_clear_cache', base)
        self.assertNotIn('-DCONFIG_LWIP_CHECK_THREAD_SAFETY=0', config)
