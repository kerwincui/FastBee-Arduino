"""Generate source-bound action/type predicates using actual C++, never devices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path('docs/generated-action-target-contracts.md')
MARKER = '<!-- generated: scripts/generate-action-contracts.py; do not edit tables -->'
SOURCES = ('include/core/PeripheralActionContract.h', 'include/core/PeripheralConfig.h',
           'include/core/PeripheralTypes.h', 'include/core/PeripheralExecution.h',
           'include/core/ActionContractMetadata.h', 'include/core/ActionValueContract.h',
           'include/core/SensorDriverMetadata.h', 'include/peripherals/drivers/ADS1115Contract.h',
           'include/network/ActionContractJson.h', 'src/network/RuleActionContracts.cpp',
           'src/core/Pca9685ConfigSupport.cpp',
           'include/core/H1ParameterContract.h', 'include/core/H1ParameterJson.h',
           'src/core/H1ParameterJson.cpp', 'include/peripherals/drivers/PCA9685Contract.h',
           'include/core/FeatureFlags.h', 'include/core/ChipConfig.h',
           'include/systems/GpioConfig.h', 'test/mocks/Arduino.h',
           'scripts/generate-action-contracts.py')


def normalize(text):
    return text.removeprefix('\ufeff').replace('\r\n', '\n')


def source_hashes(root):
    return {name: hashlib.sha256(normalize((root / name).read_text(encoding='utf-8')).encode()).hexdigest() for name in SOURCES}


def enum_names(text, name):
    # Names only: the C++ compiler resolves implicit numbers and expressions.
    clean = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*', '', text)
    matches = re.findall(r'\benum\s+class\s+' + re.escape(name) + r'(?:\s*:\s*[A-Za-z0-9_]+)?\s*\{([^{}]*)\}', clean)
    if len(matches) != 1:
        raise ValueError('Expected one complete enum: ' + name)
    names = []
    for entry in matches[0].split(','):
        entry = entry.strip()
        if not entry:
            continue
        match = re.fullmatch(r'([A-Za-z_][A-Za-z_0-9]*)(?:\s*=\s*[A-Za-z_0-9+() -]+)?', entry)
        if not match or match[1] in names:
            raise ValueError('Unsupported or duplicate enum declaration: ' + name)
        names.append(match[1])
    if not names:
        raise ValueError('Empty enum: ' + name)
    return names


def generate(root=ROOT):
    root = Path(root).resolve()
    before = source_hashes(root)
    actions = enum_names((root / 'include/core/PeripheralExecution.h').read_text(encoding='utf-8'), 'ExecActionType')
    types = enum_names((root / 'include/core/PeripheralTypes.h').read_text(encoding='utf-8'), 'PeripheralType')
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise RuntimeError('C++17 compiler required; use the Host test toolchain')
    json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
    if not json_root:
        candidates = sorted((root / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not candidates:
            raise RuntimeError('Installed ArduinoJson headers required; no download is performed')
        json_root = str(candidates[0].parent)
    def rows(enum, names):
        return ','.join('{static_cast<unsigned>(' + enum + '::' + name + '),"' + name + '"}' for name in names)
    program = r'''
#include <cmath>
using std::isnan; using std::isinf;
#include <cstdio>
#include "core/PeripheralActionContract.h"
#include "core/ActionContractMetadata.h"
struct Entry { unsigned value; const char* name; };
int main() {
    const Entry actions[] = { ACTION_ENTRIES };
    const Entry types[] = { TYPE_ENTRIES };
    for (const auto& type : types) std::printf("T %u %s\n", type.value, type.name);
    for (const auto& action : actions) {
        if (action.value > 255) return 2;
        const bool required = FastBee::PeripheralActionContract::requiresPeripheralTarget(action.value);
        std::printf("A %u %s %u\n", action.value, action.name, unsigned(required));
        if (!required) continue; // default-true is NOT a peripheral capability
        for (unsigned mode = 0; mode < 4; ++mode) for (const auto& type : types) {
            PeripheralConfig target; target.type = static_cast<PeripheralType>(type.value);
            target.params.gpio.ioSource = mode & 1; target.params.gpio.passive = (mode & 2) != 0;
            if (action.value == static_cast<unsigned>(ExecActionType::ACTION_THERMOSTAT)) {
                // Enumerate a declared admissible target, not the default
                // unconfigured pinCount=0/LOW which supports no controller.
                target.pinCount = 1; target.params.gpio.invertLogic = false;
                target.params.gpio.initialState = GPIOState::STATE_HIGH;
            }
            if (FastBee::PeripheralActionContract::matchesPeripheralTarget(action.value, target))
                std::printf("M %u %u %u\n", action.value, mode, type.value);
        }
    }
    auto describe = [](const char* scenario, ExecActionType action, const PeripheralConfig& target,
                       const char* command=nullptr, const PeripheralConfig* provider=nullptr) {
        const auto value=FastBee::describeActionContract(static_cast<uint8_t>(action), &target, command, provider);
        std::printf("C %s %u %u %s %s %s %s %s %u %s\n", scenario, unsigned(action), unsigned(value.covered),
            value.schema,value.result,value.idempotence,value.replay,value.timeoutScope,value.timeoutBudgetMs,value.fault);
    };
    PeripheralConfig target;
    target.type=PeripheralType::DAC;
    describe("DAC", ExecActionType::ACTION_SET_DAC, target);
    target=PeripheralConfig{}; target.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    target.params.gpio.ioSource=0; target.params.gpio.passive=false;
    describe("GPIO_HIGH", ExecActionType::ACTION_HIGH, target);
    describe("GPIO_TOGGLE", ExecActionType::ACTION_GPIO_TOGGLE, target);
    PeripheralConfig provider; provider.type=PeripheralType::IO_EXPANDER; provider.id="mcp";
    std::strcpy(provider.params.ioExpander.driver,"MCP23017");
    target.params.gpio.ioSource=1; std::strcpy(target.params.gpio.providerId,"mcp");
    describe("MCP23017_HIGH", ExecActionType::ACTION_HIGH, target, nullptr, &provider);
    target=PeripheralConfig{}; target.type=PeripheralType::GPIO_DIGITAL_OUTPUT; target.pinCount=1;
    target.params.gpio.ioSource=0; target.params.gpio.passive=false; target.params.gpio.invertLogic=false;
    target.params.gpio.initialState=GPIOState::STATE_HIGH;
    describe("THERMOSTAT_STATUS", ExecActionType::ACTION_THERMOSTAT, target);
    target=PeripheralConfig{}; target.type=PeripheralType::SDIO;
    describe("SD_RECORDER_STATUS", ExecActionType::ACTION_SD_RECORD, target);
    target=PeripheralConfig{}; target.type=PeripheralType::SENSOR_GENERIC;
    std::strcpy(target.params.sensor.driver,"ADS1115");
    target.params.sensor.mux=4; target.params.sensor.fullScaleMv=4096; target.params.sensor.dataRate=128;
    describe("ADS1115_128SPS", ExecActionType::ACTION_SENSOR_READ, target);
    target.params.sensor.scanMask=240; target.params.sensor.dataRate=8;
    describe("ADS1115_SCAN4_8SPS", ExecActionType::ACTION_SENSOR_READ, target);
    target.params.sensor.scanMask=0; target.params.sensor.dataRate=128;
    target.type=PeripheralType::PCA9685_PWM;
    describe("PCA9685_DUTY_PARSED", ExecActionType::ACTION_CALL_PERIPHERAL, target,"duty");
    describe("PCA9685_STOP_PARSED", ExecActionType::ACTION_CALL_PERIPHERAL, target,"stop");
    describe("PCA9685_UNCLASSIFIED", ExecActionType::ACTION_CALL_PERIPHERAL, target);
}
'''.replace('ACTION_ENTRIES', rows('ExecActionType', actions)).replace('TYPE_ENTRIES', rows('PeripheralType', types))
    env = dict(os.environ)
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='fastbee-action-catalog-') as directory:
        temp = Path(directory)
        source = temp / 'catalog.cpp'
        binary = temp / ('catalog.exe' if os.name == 'nt' else 'catalog')
        source.write_text(program, encoding='utf-8')
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'test/mocks'),
                   '-I'+str(root/'include'), '-I'+json_root, str(source), '-o', str(binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60, env=env)
        if built.returncode:
            raise RuntimeError('Contract compilation failed: ' + built.stdout + built.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10, env=env)
        if ran.returncode:
            raise RuntimeError('Contract execution failed: ' + ran.stdout + ran.stderr)
    if before != source_hashes(root):
        raise RuntimeError('Source changed during generation; no report written')
    report = dict(scope='Host C++ target predicates and declared action semantics only', firmwareValidated=False, hardwareValidated=False,
                  sourceHashes=before, types=[], actions=[], contracts=[])
    by_action = {}
    for line in ran.stdout.splitlines():
        parts = line.split()
        if parts[0] == 'T':
            report['types'].append(dict(id=int(parts[1]), name=parts[2]))
        elif parts[0] == 'A':
            value = dict(id=int(parts[1]), name=parts[2], requiresPeripheral=parts[3]=='1', modes=[[],[],[],[]])
            if value['id'] in by_action:
                raise RuntimeError('Duplicate action ID')
            by_action[value['id']] = value
            report['actions'].append(value)
        elif parts[0] == 'M':
            by_action[int(parts[1])]['modes'][int(parts[2])].append(int(parts[3]))
        elif parts[0] == 'C' and len(parts) == 11:
            report['contracts'].append(dict(scenario=parts[1], action=int(parts[2]), covered=parts[3]=='1',
                schema=parts[4], result=parts[5], idempotence=parts[6], replay=parts[7],
                timeoutScope=parts[8], timeoutBudgetMs=int(parts[9]) or None, fault=parts[10]))
        else:
            raise RuntimeError('Unexpected harness output')
    if len(report['types']) != len(types) or len(report['actions']) != len(actions):
        raise RuntimeError('Incomplete generated catalog')
    if len({value['id'] for value in report['types']}) != len(types):
        raise RuntimeError('Duplicate peripheral ID requires explicit alias handling')
    return report


def render(report):
    lines = [MARKER, '# 自动生成的动作与外设目标契约', '',
             '> 仅执行生产头文件的主机 C++ 纯目标谓词；不初始化驱动、不执行动作、不访问总线/设备。', '',
             '刷新：python scripts/generate-action-contracts.py --write；只读复验：python scripts/generate-action-contracts.py --check。Windows 使用 PlatformIO penv Python，并将 Host C++ 工具链加入 PATH。',
             'Host unit 的 test_action_contract_catalog 检查表格和来源是否过期。需要现有 ArduinoJson 头文件；不下载或更改 SDK，不使用共享构建目录。', '',
             '## 边界', '',
             '- “需要外设目标”与“目标类型匹配”是两个不同谓词；对不需要外设的动作，不将默认 true 解释为支持所有外设。',
             '- 数字列表只表示目标类型谓词放行，不代表型号驱动已注册、构建开关启用、实例存在、资源准入或参数/权限/安全条件通过。',
             '- 通用动作 10 仍须逐命令校验；保留号 20、退役号 33 和非自动化恢复出厂动作 7 不能据本表宣称可执行。',
             '- 模式 0 为本机普通配置；模式 1 为 ioSource=1；模式 2 为 passive=true；模式 3 为两者同时。非 GPIO 类型不因此获得扩展器/PWM 能力。',
             '- 动作 39 温控的枚举样本额外指定单引脚、非反相、初始 HIGH；匹配数字输出不表示任意 GPIO 配置可作温控，也不代替来源、时效、独占及接线校验。',
             '- 主机谓词证据不是九环境固件或板卡/驱动/场景实物证据；不自动提升任何 E3～E5 状态。引脚、字段质量、迁移及运行资源矩阵仍需独立补齐。', '',
             '## 动作目标', '',
             '| ID | 枚举 | 需要外设目标 | 普通模式匹配的类型 ID | 模式 1/2/3 相对普通模式变化 |',
             '| ---: | --- | --- | --- | --- |']
    for action in sorted(report['actions'], key=lambda item: item['id']):
        base = set(action['modes'][0])
        changes = []
        for mode in range(1,4):
            current = set(action['modes'][mode]); added, removed = sorted(current-base), sorted(base-current)
            if added or removed:
                changes.append(str(mode) + ': +' + ','.join(map(str,added)) + ' / -' + ','.join(map(str,removed)))
        matches = ', '.join(map(str,sorted(base))) if action['requiresPeripheral'] else '不适用，另有目标命名空间/系统约束'
        if action['name'] == 'ACTION_CALL_PERIPHERAL':
            matches = '仅通用目标检查；逐命令能力另验，不列为全类型支持'
        elif action['name'] == 'ACTION_THERMOSTAT':
            matches += '（单引脚、非反相、初始 HIGH；另有温控来源与独占约束）'
        elif action['name'] == 'ACTION_SD_RECORD':
            matches += '（SDSPI既有owner；唯一数值采集来源；单记录器；执行只读状态）'
        lines.append('| ' + ' | '.join([str(action['id']), action['name'], '是' if action['requiresPeripheral'] else '否', matches or '无', '; '.join(changes) or '无']) + ' |')
    lines += ['', '## 外设类型定义', '', '枚举存在不等于驱动可用；UNCONFIGURED/保留号也列出以避免误复用 ID。数值由编译器解析，不按行号或人工递增猜测。', '', '| ID | 枚举 |', '| ---: | --- |']
    for value in sorted(report['types'], key=lambda item: item['id']):
        lines.append('| ' + str(value['id']) + ' | ' + value['name'] + ' |')
    lines += ['', '## 已描述的动作语义样本', '',
              '以下由 `ActionContractMetadata.h` 的实际纯描述函数生成；规则预检 JSON 和 Web 检查说明消费同一描述。场景只是明确配置的样本，不是全组合或运行许可。',
              'schema 是现有参数契约的名称，不是完整 JSON Schema；具体值仍由生产解析器/驱动校验。PCA9685 样本的 PARSED 表示调用方先通过字面量解析器；动态命令、prepare/start 不据此获得支持。',
              'unknown 保留未知；timeoutBudgetMs 为空表示没有该预算字段，不表示零等待或无限超时。ADS1115 预算只限制转换等待，不限制 I2C 准入及整个动作耗时。SDK/本机接受、采样、状态均不是物理负载反馈。', '',
              '| 配置样本 / 动作 | 参数契约名 | 结果 | 幂等边界 | 重放策略 | 时间范围 / 预算 ms | 故障边界 |',
              '| --- | --- | --- | --- | --- | --- | --- |']
    for item in report['contracts']:
        lines.append('| ' + ' | '.join([item['scenario'] + ' / ' + str(item['action']), item['schema'],
            item['result'], item['idempotence'], item['replay'], item['timeoutScope'] + ' / ' +
            (str(item['timeoutBudgetMs']) if item['timeoutBudgetMs'] is not None else 'null'), item['fault']]) + ' |')
    lines += ['', '## 来源指纹', '', '以下仅是列出的生产头文件、Host mock 与生成器的归一化 UTF-8 文本 SHA256（去 BOM、CRLF 转 LF）；不是完整工具链、固件镜像或发布候选摘要。', '', '| 来源 | 文本 SHA256 |', '| --- | --- |']
    for name, digest in report['sourceHashes'].items():
        lines.append('| ' + name + ' | ' + digest + ' |')
    return '\n'.join(lines) + '\n'


def check_report(expected, path):
    if not path.is_file() or normalize(path.read_text(encoding='utf-8')) != expected:
        raise RuntimeError('Action target catalog missing/stale; run scripts/generate-action-contracts.py --write')


def write_report(expected, path):
    if path.exists() and not normalize(path.read_text(encoding='utf-8')).startswith(MARKER + '\n'):
        raise RuntimeError('Refusing to overwrite a non-generated document')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(expected, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--write', action='store_true')
    mode.add_argument('--stdout', action='store_true')
    mode.add_argument('--check', action='store_true')
    args = parser.parse_args()
    result = generate()
    text = render(result)
    if args.stdout:
        print(text, end='')
    elif args.write:
        write_report(text, ROOT / OUTPUT)
        print(json.dumps(dict(status='GENERATED', actions=len(result['actions']), types=len(result['types']), output=OUTPUT.as_posix())))
    else:
        check_report(text, ROOT / OUTPUT)
        print('Action target catalog current; Host predicates only, no devices')
