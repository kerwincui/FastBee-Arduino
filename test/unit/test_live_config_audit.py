"""Host-only contracts for the five-device live configuration audit."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
AUDIT = ROOT / "scripts" / "audit-live-device-config.mjs"
NODE = os.environ.get("FASTBEE_NODE", "node")

DEVICES = (
    ("ESP32-S3-F16R8", "esp32s3-F16R8", "COM15"),
    ("ESP32-C3-F4R0", "esp32c3-F4R0", "COM4"),
    ("ESP32-C6-F4R0", "esp32c6-F4R0", "COM11"),
    ("ESP32-F4R0", "esp32-F4R0", "COM16"),
    ("ESP32-S3-F8R0", "esp32s3-F8R0", "COM7"),
)


def write_json(target: pathlib.Path, value: object) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(value), encoding="utf-8")


def make_deployment(
    root: pathlib.Path,
    run_name: str,
    finished_at: str,
    *,
    build_only: bool = False,
    duplicate_hashes: bool = False,
) -> pathlib.Path:
    run_dir = root / run_name
    results = []
    source_map = {}
    for index, (name, environment, port) in enumerate(DEVICES, start=1):
        source = run_dir / f"source-{index}"
        source.mkdir(parents=True, exist_ok=True)
        report = run_dir / f"device-{index}.json"
        write_json(
            report,
            {
                "status": "success",
                "dryRun": False,
                "buildOnly": build_only,
                "environment": environment,
                "port": port,
                "bundlePath": str(source),
                "validation": {"peripherals": 1, "rules": 1},
            },
        )
        source_hash = ("1" if duplicate_hashes else format(index, "x")).zfill(64)
        source_meta = {"path": str(source), "sha256": source_hash}
        source_map[name] = source_meta
        results.append(
            {
                "device": name,
                "environment": environment,
                "port": port,
                "source": source_meta,
                "status": "built" if build_only else "deployed",
                "report": str(report),
            }
        )
    summary = run_dir / f"{run_name}-summary.json"
    write_json(
        summary,
        {
            "runId": run_name,
            "finishedAt": finished_at,
            "dryRun": False,
            "buildOnly": build_only,
            "order": [item[0] for item in DEVICES],
            "sourceSha256": source_map,
            "results": results,
        },
    )
    return summary


def resolve_with_node(*, directory: pathlib.Path | None = None, summary: pathlib.Path | None = None) -> dict:
    module_uri = AUDIT.as_uri()
    expression = (
        "import {resolveDeploymentContext} from " + json.dumps(module_uri) + ";"
        "const context=resolveDeploymentContext({"
        "deploymentDir:process.env.FB_TEST_DEPLOY_DIR||undefined,"
        "deploymentSummary:process.env.FB_TEST_DEPLOY_SUMMARY||''});"
        "console.log(JSON.stringify({path:context.summaryPath,runId:context.summary.runId,"
        "deviceCount:context.bindings.size}));"
    )
    environment = os.environ.copy()
    if directory is not None:
        environment["FB_TEST_DEPLOY_DIR"] = str(directory)
    if summary is not None:
        environment["FB_TEST_DEPLOY_SUMMARY"] = str(summary)
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", expression],
        cwd=ROOT,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(result.stderr or result.stdout)
    return json.loads(result.stdout)


def evaluate_with_node(expression: str) -> dict:
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", expression],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(result.stderr or result.stdout)
    return json.loads(result.stdout)


class LiveConfigAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = AUDIT.read_text(encoding="utf-8")

    def test_explicit_summary_and_latest_complete_directory_are_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            valid = make_deployment(root, "valid", "2026-09-11T01:00:00Z")
            make_deployment(root, "newer-build-only", "2026-09-11T02:00:00Z", build_only=True)

            selected = resolve_with_node(directory=root)
            self.assertEqual(pathlib.Path(selected["path"]), valid.resolve())
            self.assertEqual(selected["runId"], "valid")
            self.assertEqual(selected["deviceCount"], 5)

            explicit = resolve_with_node(summary=valid)
            self.assertEqual(pathlib.Path(explicit["path"]), valid.resolve())

    def test_duplicate_configuration_hashes_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            summary = make_deployment(
                pathlib.Path(temporary),
                "duplicate",
                "2026-09-11T01:00:00Z",
                duplicate_hashes=True,
            )
            with self.assertRaises(AssertionError) as raised:
                resolve_with_node(summary=summary)
            self.assertIn("distinct valid SHA-256", str(raised.exception))

    def test_runtime_status_endpoints_and_assertions_are_registered(self):
        for endpoint in (
            "'/api/auth/session'",
            "'/api/network/status'",
            "'/api/mqtt/status'",
            "'/api/modbus/status?compact=1'",
        ):
            self.assertIn(endpoint, self.source)
        self.assertIn("session.sessionValid === true", self.source)
        self.assertIn("MQTT is not connected but status API returned no failure/progress reason", self.source)
        self.assertIn("Modbus status task count", self.source)

    def test_config_comparisons_are_explicit_and_secrets_are_not_reported(self):
        self.assertIn("if (!hasOwn(explicitMqtt, key)) continue", self.source)
        self.assertIn("if (!hasOwn(explicitNetwork, sourceKey)) continue", self.source)
        self.assertIn("secretResponseSemantics", self.source)
        self.assertIn("body: redact(response.body)", self.source)
        self.assertNotIn("secretMatchesStagedSource", self.source)
        self.assertNotIn("passwordMatchesStagedSource", self.source)
        self.assertNotIn("finalReportDir", self.source)
        self.assertNotIn("20260911-115819", self.source)

    def test_classic_no_psram_and_secret_response_semantics(self):
        module_uri = AUDIT.as_uri()
        result = evaluate_with_node(
            "import {applyDeploymentTransforms,secretResponseSemantics} from "
            + json.dumps(module_uri)
            + ";const transformed=applyDeploymentTransforms({}, {memoryProfile:'classic-no-psram',networkProfile:'none'},"
            + "{'protocol.json':{mqtt:{enabled:true,publishTopics:[{topic:'up'}],subscribeTopics:[{topic:'down'}]}},"
            + "'network.json':{staSSID:'ssid',enableMDNS:true,autoFailover:true}});"
            + "console.log(JSON.stringify({mqtt:transformed.protocol.mqtt,network:transformed.network,"
            + "masked:secretResponseSemantics('********',true),omitted:secretResponseSemantics('',true),"
            + "exposed:secretResponseSemantics('plain-secret',true),unexpected:secretResponseSemantics('***',false)}));"
        )
        self.assertFalse(result["mqtt"]["enabled"])
        self.assertEqual(result["mqtt"]["publishTopics"], [])
        self.assertEqual(result["mqtt"]["subscribeTopics"], [])
        self.assertFalse(result["network"]["enableMDNS"])
        self.assertFalse(result["network"]["autoFailover"])
        self.assertTrue(result["masked"]["acceptable"])
        self.assertTrue(result["omitted"]["acceptable"])
        self.assertFalse(result["exposed"]["acceptable"])
        self.assertFalse(result["unexpected"]["acceptable"])

    def test_analysis_accepts_disabled_templates_generated_client_id_and_omitted_core_feature(self):
        module_uri = AUDIT.as_uri()
        expression = """
import {analyse} from %s;
const ok=(data)=>({ok:true,status:200,durationMs:1,body:{success:true,data}});
const disabledTasks=[{enabled:false,name:'one'},{enabled:false,name:'two'}];
const expected={
  effective:{protocol:{mqtt:{enabled:true,server:'broker',port:1883,clientId:''}},network:{}},
  explicit:{mqtt:{enabled:true,server:'broker',port:1883,clientId:'',password:'source-secret'},network:{staPassword:'source-wifi-secret'}},
  summary:{identity:{deviceName:'fixture',productNumber:1,deviceId:''},peripheralIds:[],ruleIds:[],
    peripheralCount:0,enabledPeripheralCount:0,ruleCount:0,enabledRuleCount:0,
    enabledFeatures:['mdns','webServer'],modbusEnabled:true,mqttEnabled:true,
    modbusConfiguredTaskCount:2,modbusConfiguredEnabledTaskCount:0,
    modbusStatusTaskCount:0,modbusStatusEnabledTaskCount:0}
};
const responses={
  session:ok({sessionValid:true}),device:ok({deviceName:'fixture',productNumber:1,deviceId:'generated-device'}),
  features:ok({features:[{key:'mdns',compiled:true,userEnabled:true,active:true,runtimeDisabled:false}],summary:{runtimeDegraded:false}}),
  peripherals:ok([]),rules:ok([]),
  protocol:ok({mqtt:{enabled:true,server:'broker',port:1883,clientId:'generated-client',password:'********'},
    modbusRtu:{enabled:true,master:{tasks:disabledTasks,devices:[]}}}),
  network:ok({network:{},sta:{password:'********'},ap:{},advanced:{},status:{connected:true,ipAddress:'',ssid:'',rssi:0}}),
  networkStatus:ok({connected:true,ipAddress:'192.0.2.1',ssid:'fixture-net',rssi:-50}),
  system:ok({networkConnected:true,ipAddress:'192.0.2.1'}),mqttStatus:ok({enabled:true,connected:false,clientId:'generated-client',server:'broker',port:1883,statusReason:'connecting'}),
  modbusStatus:ok({status:'stopped',taskCount:0,health:{enabledTaskCount:0,warnings:[]}})
};
const result=analyse({key:'fixture',ip:'192.0.2.1'},expected,responses);
console.log(JSON.stringify(result));
""" % json.dumps(module_uri)
        result = evaluate_with_node(expression)
        self.assertTrue(result["passed"], result["mismatches"])
        self.assertEqual(result["modbus"]["taskCount"], 2)
        self.assertEqual(result["modbusStatus"]["taskCount"], 0)
        self.assertEqual(result["configuredFeatures"], ["mdns"])
        self.assertTrue(result["network"]["configSnapshot"]["stale"])
        self.assertEqual(result["network"]["ipAddress"], "192.0.2.1")
        serialized = json.dumps(result)
        self.assertNotIn("source-secret", serialized)
        self.assertNotIn("source-wifi-secret", serialized)

    def test_invalid_success_json_is_retried_and_never_analysed_as_empty_features(self):
        module_uri = AUDIT.as_uri()
        retry_result = evaluate_with_node(
            "import {requestJson} from " + json.dumps(module_uri) + ";"
            + "let calls=0;globalThis.fetch=async()=>{calls++;return new Response(calls<3?'not-json':'{\\\"success\\\":true}',{status:200});};"
            + "const result=await requestJson('http://fixture',{retryDelayMs:0},2);console.log(JSON.stringify({calls,result}));"
        )
        self.assertEqual(retry_result["calls"], 3)
        self.assertTrue(retry_result["result"]["ok"])

        failure_result = evaluate_with_node(
            "import {requestJson} from " + json.dumps(module_uri) + ";"
            + "let calls=0;globalThis.fetch=async()=>{calls++;return new Response('',{status:200});};"
            + "const result=await requestJson('http://fixture',{retryDelayMs:0},1);console.log(JSON.stringify({calls,result}));"
        )
        self.assertEqual(failure_result["calls"], 2)
        self.assertFalse(failure_result["result"]["ok"])
        self.assertIn("empty JSON response body", failure_result["result"]["error"])
        self.assertIsNone(failure_result["result"]["body"])

        expression = """
import {analyse} from %s;
const ok=(data)=>({ok:true,status:200,durationMs:1,body:{success:true,data}});
const expected={effective:{protocol:{mqtt:{enabled:false}},network:{}},explicit:{mqtt:{},network:{}},
 summary:{identity:{deviceName:'fixture',productNumber:1,deviceId:'id'},peripheralIds:[],ruleIds:[],
 peripheralCount:0,enabledPeripheralCount:0,ruleCount:0,enabledRuleCount:0,enabledFeatures:['webServer'],
 modbusEnabled:false,mqttEnabled:false,modbusConfiguredTaskCount:0,modbusConfiguredEnabledTaskCount:0,
 modbusStatusTaskCount:0,modbusStatusEnabledTaskCount:0}};
const responses={session:ok({sessionValid:true}),device:ok({deviceName:'fixture',productNumber:1,deviceId:'id'}),
 features:{ok:true,status:200,durationMs:1,body:null,error:'empty JSON response body'},peripherals:ok([]),rules:ok([]),
 protocol:ok({mqtt:{enabled:false},modbusRtu:{enabled:false,master:{tasks:[],devices:[]}}}),
 network:ok({network:{},sta:{},ap:{},advanced:{},status:{connected:true,ipAddress:''}}),
 networkStatus:ok({connected:true,ipAddress:'192.0.2.1'}),system:ok({networkConnected:true,ipAddress:'192.0.2.1'}),
 mqttStatus:ok({enabled:false,connected:false,statusReason:'disabled'}),
 modbusStatus:ok({status:'stopped',taskCount:0,health:{enabledTaskCount:0,warnings:[]}})};
console.log(JSON.stringify(analyse({key:'fixture',ip:'192.0.2.1'},expected,responses)));
""" % json.dumps(module_uri)
        analysis = evaluate_with_node(expression)
        self.assertFalse(analysis["passed"])
        self.assertIsNone(analysis["configuredFeatures"])
        self.assertTrue(any("features endpoint failed" in item for item in analysis["mismatches"]))
        self.assertFalse(any("user-enabled feature set" in item for item in analysis["mismatches"]))

    def test_authoritative_network_status_mismatch_still_fails(self):
        module_uri = AUDIT.as_uri()
        expression = """
import {analyse} from %s;
const ok=(data)=>({ok:true,status:200,durationMs:1,body:{success:true,data}});
const expected={effective:{protocol:{mqtt:{enabled:false}},network:{}},explicit:{mqtt:{},network:{}},
 summary:{identity:{deviceName:'fixture',productNumber:1,deviceId:'id'},peripheralIds:[],ruleIds:[],
 peripheralCount:0,enabledPeripheralCount:0,ruleCount:0,enabledRuleCount:0,enabledFeatures:[],
 modbusEnabled:false,mqttEnabled:false,modbusConfiguredTaskCount:0,modbusConfiguredEnabledTaskCount:0,
 modbusStatusTaskCount:0,modbusStatusEnabledTaskCount:0}};
const responses={session:ok({sessionValid:true}),device:ok({deviceName:'fixture',productNumber:1,deviceId:'id'}),
 features:ok({features:[],summary:{runtimeDegraded:false}}),peripherals:ok([]),rules:ok([]),
 protocol:ok({mqtt:{enabled:false},modbusRtu:{enabled:false,master:{tasks:[],devices:[]}}}),
 network:ok({network:{},sta:{},ap:{},advanced:{},status:{connected:true,ipAddress:''}}),
 networkStatus:ok({connected:true,ipAddress:'192.0.2.99'}),system:ok({networkConnected:true,ipAddress:'192.0.2.99'}),
 mqttStatus:ok({enabled:false,connected:false,statusReason:'disabled'}),
 modbusStatus:ok({status:'stopped',taskCount:0,health:{enabledTaskCount:0,warnings:[]}})};
console.log(JSON.stringify(analyse({key:'fixture',ip:'192.0.2.1'},expected,responses)));
""" % json.dumps(module_uri)
        analysis = evaluate_with_node(expression)
        self.assertFalse(analysis["passed"])
        self.assertTrue(any("network status endpoint IP" in item for item in analysis["mismatches"]))
        self.assertTrue(any("system status endpoint IP" in item for item in analysis["mismatches"]))


if __name__ == "__main__":
    unittest.main()
