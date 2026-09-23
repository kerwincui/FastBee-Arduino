"""Compile actual patched response/request/close functions with fault injection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_libdeps_patch_contract import PATCH_MODULE
from test.unit.test_screen_panel_contract import braced_block

ROOT=Path(__file__).resolve().parents[2]

def method(source, signature):
    return signature+" {"+braced_block(source, signature)+"}\n"

class WebPeerDrainTests(unittest.TestCase):
    def test_partial_upgrade_is_rejected(self):
        with self.assertRaises(PATCH_MODULE.PatchError):
            PATCH_MODULE._patch_peer_drained_completion("FastBee patch: peer-drained completion")

    def run_harness(self, eager=False):
        candidates=list((ROOT/".pio/libdeps").glob("*/ESPAsyncWebServer/src/WebResponses.cpp"))
        if not candidates: self.skipTest("installed libraries required")
        library=candidates[0].parent
        tcp=next((ROOT/".pio/libdeps").glob("*/AsyncTCP/src/AsyncTCP.cpp"))
        compiler=shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        environment=dict(os.environ)
        environment["PATH"]=str(Path(compiler).parent)+os.pathsep+environment.get("PATH","")
        with tempfile.TemporaryDirectory(prefix="fastbee-peer-drain-") as directory:
            temp=Path(directory)
            target=temp/"WebResponses.cpp";shutil.copyfile(candidates[0],target)
            PATCH_MODULE._patch_web_responses(target)
            response=target.read_text(encoding="utf-8")
            self.assertFalse(PATCH_MODULE._patch_web_responses(target))
            request,_=PATCH_MODULE._patch_web_request_source_text((library/"WebRequest.cpp").read_text(encoding="utf-8"))
            again,changed=PATCH_MODULE._patch_web_request_source_text(request)
            self.assertFalse(changed);self.assertEqual(request,again)
            if eager:
                response=response.replace("if (request->client()->space() == TCP_SND_BUF) _state = RESPONSE_END;", "_state = RESPONSE_END;")
            (temp/"production_response_batch.inc").write_text("".join(method(response,
                f"size_t {name}::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time)")
                for name in ("AsyncBasicResponse","AsyncAbstractResponse")),encoding="utf-8")
            methods="".join(method(request, signature) for signature in (
                "void AsyncWebServerRequest::_onAck(size_t len, uint32_t time)",
                "void AsyncWebServerRequest::_onPoll()",
                "void AsyncWebServerRequest::_onTimeout(uint32_t time)",
                "void AsyncWebServerRequest::_onDisconnect()"))
            methods+=method(tcp.read_text(encoding="utf-8"),"static err_t _tcp_close_api(struct tcpip_api_call_data *api_call_msg)")
            (temp/"production_peer_drain.inc").write_text(methods,encoding="utf-8")
            executable=temp/"test.exe"
            built=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",f"-I{temp}",
                str(ROOT/"test/unit/cpp/web_peer_drain_harness.cpp"),"-o",str(executable)],
                capture_output=True,text=True,timeout=60,env=environment)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            return subprocess.run([str(executable)],capture_output=True,text=True,timeout=15,env=environment)

    def test_actual_functions_wait_for_peer_and_release_ownership(self):
        result=self.run_harness()
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_eager_completion_mutation_is_detected(self):
        result=self.run_harness(eager=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("Assertion failed",result.stderr)

if __name__=="__main__": unittest.main()
