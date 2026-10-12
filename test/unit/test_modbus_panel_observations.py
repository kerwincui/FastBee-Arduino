import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ModbusPanelObservationTests(unittest.TestCase):
    def test_batch_merge_flat_members_preserve_offsets_and_task_identity(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")
        start = source.index("// 按 (slaveId, functionCode, startAddress)")
        end = source.index('LOG_INFOF("[Modbus] Batch:', start)
        planner = source[start:end]
        harness = r'''
#include <cassert>
#include <cstdint>
namespace Protocols {
constexpr uint8_t MODBUS_MAX_POLL_TASKS = 8;
constexpr uint16_t MODBUS_ONESHOT_BUFFER_SIZE = 32;
constexpr uint16_t MODBUS_MAX_REGISTERS_PER_READ = 125;
constexpr uint16_t MODBUS_MERGE_MAX_GAP = 4;
}
struct PollTask { uint8_t slaveAddress, functionCode; uint16_t startAddress, quantity; };
struct { struct { PollTask tasks[8]; } master; } config;
void verify(bool sameSlave) {
    uint8_t candCount = 8;
    uint8_t cand[8] = {7, 3, 1, 5, 0, 6, 2, 4};
    for (uint8_t i = 0; i < 8; ++i)
        config.master.tasks[i] = {static_cast<uint8_t>(sameSlave ? 1 : i / 2 + 1), 3,
                                 static_cast<uint16_t>(i * 3), 2};
    PLANNER
    static_assert(sizeof(segs) + sizeof(members) <= 112, "batch tables must remain bounded");
    assert(segCount == (sameSlave ? 1 : 4));
    bool seen[8] = {};
    for (uint8_t si = 0; si < segCount; ++si) {
        const auto& seg = segs[si];
        assert(seg.memberStart + seg.memberCount <= candCount);
        for (uint8_t mi = 0; mi < seg.memberCount; ++mi) {
            const auto& member = members[seg.memberStart + mi];
            const auto& task = config.master.tasks[member.taskIdx];
            assert(!seen[member.taskIdx]);
            seen[member.taskIdx] = true;
            assert(task.slaveAddress == seg.slave);
            assert(seg.start + member.offset == task.startAddress);
            assert(member.qty == task.quantity);
            assert(member.offset + member.qty <= seg.qty);
        }
    }
    for (bool present : seen) assert(present);
}
int main() { verify(false); verify(true); }
'''.replace("PLANNER", planner)
        with tempfile.TemporaryDirectory(prefix="fastbee-modbus-batch-") as directory:
            temporary = pathlib.Path(directory)
            cpp = temporary / "batch.cpp"
            cpp.write_text(harness, encoding="utf-8")
            executable = temporary / ("batch.exe" if os.name == "nt" else "batch")
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    str(cpp), "-o", str(executable)], capture_output=True,
                                   text=True, timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_production_mapping_quality_history_and_stop_invalidation(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")
        signatures = [
            "void ModbusHandler::publishPollObservations(uint8_t taskIdx, bool sampled)",
            "void ModbusHandler::invalidatePollObservations()",
            "void ModbusHandler::end()",
        ]
        with tempfile.TemporaryDirectory(prefix="fastbee-modbus-panel-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "production_modbus_observations.inc").write_text(
                "struct MotorBusGuard {" + braced_block(source, "struct MotorBusGuard") + "};\n"
                + "\n".join(signature + "{" + braced_block(source, signature) + "}"
                            for signature in signatures), encoding="utf-8")
            executable = temporary / ("observations.exe" if os.name == "nt" else "observations")
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-DFASTBEE_ENABLE_PERIPH_EXEC=1", "-DFASTBEE_ENABLE_SCREEN_PANEL=1",
                 "-DFASTBEE_ENABLE_POINT_HISTORY=1", f"-I{temporary}", f"-I{ROOT / 'include'}",
                 str(ROOT / "test/unit/cpp/modbus_panel_observation_harness.cpp"),
                 "-o", str(executable)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("mapping, quality, history and guarded stop lifecycle passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
