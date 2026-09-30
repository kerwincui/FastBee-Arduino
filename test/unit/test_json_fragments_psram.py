"""Compile the production ESP32 fragment path and verify DRAM stays bounded."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class JsonFragmentPsramTests(unittest.TestCase):
    def test_psram_ownership_failure_partial_reads_and_fallback(self):
        compiler=shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header=next((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        with tempfile.TemporaryDirectory(prefix='fastbee-json-psram-') as directory:
            temp=Path(directory);(temp/'utils').mkdir()
            shutil.copyfile(ROOT/'test/unit/cpp/json_success_arduino_mock.h',temp/'Arduino.h')
            (temp/'utils/PsramJsonDocument.h').write_text('''#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,int); void heap_caps_free(void*);
namespace FastBee { bool psramAvailableForJson(size_t=8192); }
''',encoding='utf8')
            (temp/'ESPAsyncWebServer.h').write_text('''#pragma once
#include <Arduino.h>
class AsyncAbstractResponse {public:
virtual ~AsyncAbstractResponse()=default;
virtual bool _sourceValid() const=0;
virtual size_t _fillBuffer(uint8_t*,size_t)=0;
int _code=0;String _contentType;size_t _contentLength=0;bool _sendContentLength=false;
};
''',encoding='utf8')
            (temp/'main.cpp').write_text(r'''
#include "network/JsonFragmentsResponse.h"
#include <cassert>
#include <cstdlib>
#include <unordered_map>
#include <iostream>
static bool available=true,failAllocation=false;
static std::unordered_map<void*,size_t> allocations;
namespace FastBee {bool psramAvailableForJson(size_t){return available;}}
void* heap_caps_malloc(size_t n,int caps) {
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(failAllocation)return nullptr;
 void* p=std::malloc(n);if(p)allocations[p]=n;return p;
}
void heap_caps_free(void* p){assert(allocations.erase(p)==1);std::free(p);}
std::string drain(FastBee::JsonFragmentsResponse& response) {
 std::string result;uint8_t bytes[137];
 while(result.size()<response._contentLength) {
  size_t n=response._fillBuffer(bytes,sizeof(bytes));assert(n&&n<=sizeof(bytes));
  result.append(reinterpret_cast<char*>(bytes),n);
 }
 assert(response._fillBuffer(bytes,sizeof(bytes))==0);return result;
}
int main() {
 for(bool usePsram:{true,false}) {
  available=usePsram;
  {
   FastBee::JsonFragmentsResponse response(3);
   assert(response.allocated());std::string expected;
   for(int i=0;i<3;++i) {
    JsonDocument doc;doc["text"]=std::string(5000,'a'+i);doc["value"]=i;
    std::string value;serializeJson(doc,value);
    const char* prefix=i==0?"[":",";const char* suffix=i==2?"]":"";
    expected+=prefix;expected+=value;expected+=suffix;
    assert(response.set(i,doc,prefix,suffix));
    assert(!response.set(i,doc,prefix,suffix));
    doc.clear();
   }
   assert(response._sourceValid());assert(response._contentLength==expected.size());
   if(usePsram) {assert(stringBytes<128);assert(allocations.size()==3);}
   else {assert(stringBytes>15000);assert(allocations.empty());}
   assert(drain(response)==expected);assert(allocations.empty());
  }
  assert(stringBytes==0);
 }
 available=true;
 {
  JsonDocument doc;doc["text"]=std::string(4000,'x');
  FastBee::JsonFragmentsResponse response(1);failAllocation=true;
  assert(!response.set(0,doc));assert(!response._sourceValid());
  assert(allocations.empty());assert(stringBytes<128);failAllocation=false;
 }
 {
  JsonDocument doc;doc["text"]=std::string(4000,'x');
  FastBee::JsonFragmentsResponse response(1);assert(response.set(0,doc));
  uint8_t bytes[7];assert(response._fillBuffer(bytes,sizeof(bytes))==7);
  assert(allocations.size()==1); // disconnected before completion
 }
 assert(allocations.empty());assert(stringBytes==0);
 std::cout<<"PSRAM fragments: <128B String capacity for >15KB payload; ownership, partial reads, OOM, cancellation and fallback passed\n";
}
''',encoding='utf8')
            env=dict(os.environ);env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
            exe=temp/'test.exe'
            run=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-DESP32',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0','-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I'+str(temp),'-I'+str(ROOT/'include'),'-I'+str(json_header.parent),
                str(temp/'main.cpp'),'-o',str(exe)],capture_output=True,text=True,env=env,timeout=60)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=15)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            print(run.stdout.strip())

if __name__=='__main__':unittest.main()
