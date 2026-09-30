"""Compile the actual shared allocator with bounded simulated DRAM and PSRAM."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class TlsMemoryAllocatorTests(unittest.TestCase):
    def test_allocator_reserve_failure_overflow_and_boot_order(self):
        startup=(ROOT/'src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('FastBee::TlsMemory::ensureAllocator();',startup)
        self.assertLess(startup.index('FastBee::TlsMemory::ensureAllocator();'),startup.index('FastBeeFramework::getInstance()',startup.index('void setup()')))
        compiler=shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        with tempfile.TemporaryDirectory(prefix='fastbee-tls-memory-') as folder:
            temp=Path(folder); (temp/'mbedtls').mkdir()
            (temp/'Arduino.h').write_text('''#pragma once
#include <cstddef>
inline bool psramPresent=true;
inline bool psramFound() { return psramPresent; }
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(int*) {}
inline void portEXIT_CRITICAL(int*) {}
''')
            (temp/'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
inline size_t psramFree=1048576, largest=65536;
inline unsigned calls=0, lastCaps=0, releases=0;
inline bool allocationFails=false;
inline size_t heap_caps_get_free_size(unsigned) { return psramFree; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return largest; }
inline void* heap_caps_calloc(size_t n,size_t s,unsigned caps) {
 ++calls; lastCaps=caps; return allocationFails ? nullptr : std::calloc(n,s);
}
inline void heap_caps_free(void* p) { ++releases; std::free(p); }
''')
            (temp/'mbedtls/platform.h').write_text('''#pragma once
#define MBEDTLS_PLATFORM_MEMORY
inline unsigned installs=0;
inline bool installFails=false;
inline int mbedtls_platform_set_calloc_free(void*(*)(size_t,size_t),void(*)(void*)) {
 ++installs; return installFails ? -1 : 0;
}
''')
            program=r'''
#include "network/TlsMemory.h"
#include "protocols/WeatherSchedule.h"
#include <cassert>
namespace T=FastBee::TlsMemory;
int main() {
 installFails=true; assert(!T::ensureAllocator());
 assert(!T::usesPsram()); // Failed installation cannot reduce admission.
 installFails=false; assert(T::ensureAllocator());
 auto count=installs; assert(T::ensureAllocator() && installs==count);
#ifdef BOARD_HAS_PSRAM
 assert(T::usesPsram());
 auto p=T::calloc(1,16384); assert(p && lastCaps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); T::free(p);
 psramFree=T::PSRAM_RESERVE+16384;
 count=calls; assert(!T::calloc(1,16384) && calls==count); // No hidden DRAM fallback.
 assert(!T::usesPsram());
 psramFree=1048576; allocationFails=true;
 count=calls; assert(!T::calloc(1,16384) && calls==count+1); allocationFails=false;
 largest=32000; assert(!T::usesPsram()); largest=65536;
 psramPresent=false; assert(!T::usesPsram()); psramPresent=true;
#else
 assert(!T::usesPsram());
 auto p=T::calloc(1,16384); assert(p && lastCaps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)); T::free(p);
#endif
 auto small=T::calloc(1,32); assert(small && lastCaps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)); T::free(small);
 count=calls; assert(!T::calloc(static_cast<size_t>(-1),2) && calls==count);
 using namespace FastBee::WeatherSchedule;
 assert(admission(true,true,false,38252,22516,false,true,true)==Reason::None);
 assert(admission(true,true,false,38252,22516,false,true,false)==Reason::Memory);
 assert(admission(true,true,false,32191,22516,false,true,true)==Reason::Memory);
 assert(admission(true,true,false,24000,16384,false,false,true)==Reason::None);
 assert(admission(true,true,false,23999,16384,false,false,true)==Reason::Memory);
}
'''
            cpp=temp/'allocator.cpp'; cpp.write_text(program)
            env=dict(os.environ); env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
            for psram in (0,1):
                binary=temp/f'allocator-{psram}.exe'
                result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                    *(['-DBOARD_HAS_PSRAM'] if psram else []),f'-I{temp}',f'-I{ROOT/"include"}',
                    str(cpp),'-o',str(binary)],capture_output=True,text=True,timeout=60,env=env)
                self.assertEqual(0,result.returncode,result.stdout+result.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
                self.assertEqual(0,result.returncode,result.stdout+result.stderr)

if __name__=='__main__': unittest.main()
