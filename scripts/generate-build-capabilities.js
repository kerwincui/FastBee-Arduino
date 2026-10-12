'use strict';

// Deterministic documentation of the existing Web build capability resolver.
// No devices, runtime config, credentials or inherited CLI/env overrides.
// This is not a replacement for firmware or physical acceptance evidence.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { CAPABILITY_MACROS, parseIni, resolveBuildFlagsFromIni,
    resolveWebBuildContext } = require('./web-profile');

const ROOT = path.resolve(__dirname, '..');
const OUTPUT = 'docs/generated-build-capabilities.md';
const MARKER = '<!-- generated: scripts/generate-build-capabilities.js; do not edit tables -->';
const SOURCES = ['platformio.ini', 'scripts/web-profile.js', 'scripts/generate-build-capabilities.js'];
const normalize = (text) => String(text).replace(/^\uFEFF/, '').replace(/\r\n/g, '\n');
const hash = (text) => crypto.createHash('sha256').update(normalize(text), 'utf8').digest('hex');

function scalar(section, key, environment) {
    const values = section.get(key) || [];
    if (values.length !== 1 || !values[0].trim() || /\$\{|[|\r\n]/.test(values[0])) {
        throw new Error(environment + ': unresolved/unsupported ' + key);
    }
    return values[0].trim();
}

function collect(iniText, sourceHashes = {}) {
    const sections = parseIni(iniText);
    const names = [...sections.keys()].filter((name) => name.startsWith('env:') && name !== 'env:native');
    if (!names.length) throw new Error('No firmware environments');
    // Map parsing must not silently discard duplicate declarations. Reject
    // inheritance not handled by the shared resolver rather than invent data.
    const headers = [...normalize(iniText).matchAll(/^\s*\[([^\]\r\n]+)\]\s*$/gm)].map((m) => m[1]);
    if (new Set(headers).size !== headers.length) throw new Error('Duplicate INI section');
    const environments = names.map((sectionName) => {
        const environment = sectionName.slice(4);
        const memory = /^(esp32(?:c3|c6|s3)?)-F(\d+)R(\d+)$/.exec(environment);
        if (!memory) throw new Error('Non-canonical environment needs explicit policy: ' + environment);
        const section = sections.get(sectionName);
        if (section.has('extends')) throw new Error(environment + ': unsupported extends inheritance');
        const resolved = resolveBuildFlagsFromIni(iniText, environment);
        const context = resolveWebBuildContext({ environment, buildFlags: resolved.buildFlags, source: 'platformio.ini', argv: [] });
        const capabilityStates = Object.fromEntries(Object.entries(CAPABILITY_MACROS).map(([key, macro]) => {
            const value = context.definitions[macro];
            // Web treats undefined macros as false; C++ header defaults may not.
            if (value === undefined) return [key, 'not-declared'];
            if (!/^(?:0|1|true|false|on|off|yes|no|enabled|disabled)$/i.test(value)) {
                throw new Error(environment + ': unsupported capability expression ' + macro + '=' + value);
            }
            if (key === 'otaFs' && !/^(?:0|false|off|no|disabled)$/i.test(value))
                return [key, 'implementation-missing'];
            return [key, context.compiledCapabilities[key] ? 'included' : 'excluded'];
        }));
        return { environment, chip: memory[1], flashMB: Number(memory[2]), psramMB: Number(memory[3]),
            board: scalar(section, 'board', environment),
            partition: scalar(section, 'board_build.partitions', environment),
            capabilityStates,
            capabilityDeclarations: Object.fromEntries(Object.entries(CAPABILITY_MACROS).map(
                ([key, macro]) => [key, context.definitions[macro] === undefined ? null : context.definitions[macro]])),
            capabilityFingerprint: context.capabilityFingerprint };
    });
    return { schemaVersion: 1, evidenceLevel: 'source-configuration-only',
        runtimeVerified: false, hardwareVerified: false, sourceHashes, environments };
}

const cell = (value) => String(value).replace(/\|/g, '\\|').replace(/[\r\n]/g, ' ');
function render(report) {
    const lines = [MARKER, '# 自动生成的构建环境与 Web 能力矩阵', '',
        '> 本表从当前工作区 platformio.ini 和现有 Web 能力解析器生成，不是发布成功、实例可用或实物支持证明。', '',
        '刷新：node scripts/generate-build-capabilities.js --write；只读校验：node scripts/generate-build-capabilities.js --check。',
        '现有 validate-build-matrix.js / Host static 同时检查本表是否过期。运行时 features.json、预算、依赖、初始化和设备接线不参与本表生成。', '',
        '## 构建环境', '',
        'Flash/PSRAM 数字来自环境命名，不是设备探测；PlatformIO board 是构建定义，不是已验收的成品板修订。RGB 专用固件不在 canonical 表中。', '',
        '| 环境 | PlatformIO board | 命名 Flash MB | 命名 PSRAM MB | 分区文件 |',
        '| --- | --- | ---: | ---: | --- |'];
    for (const env of report.environments) lines.push('| ' + [env.environment, env.board, env.flashMB, env.psramMB, env.partition].map(cell).join(' | ') + ' |');
    lines.push('', '## Web 发布能力', '',
        '包含/裁剪表示显式构建宏经现有 Web resolver 解析后的资源选择。未声明表示宏未显式定义，Web resolver 按 false 处理；不能据此声称 C++ 默认宏或运行功能也关闭。',
        'otaFs 的旧宏保留分区/发布配置兼容；即使宏为 1，当前无文件系统 OTA 写入实现，有效能力仍为 false，表中标为未实现，不影响 OTA 固件升级父能力。',
        '某资源还可能有组合依赖；本表不将所有宏映射为独立页面。外设类型、驱动型号、动作目标、整板资源与 E2～E5 验收须另按生产契约核验。', '',
        '| 能力 / 构建宏 | ' + report.environments.map((env) => cell(env.environment)).join(' | ') + ' |',
        '| --- | ' + report.environments.map(() => '---').join(' | ') + ' |');
    const labels = { included: '包含', excluded: '裁剪', 'not-declared': '未声明', 'implementation-missing': '未实现（宏已声明启用）' };
    for (const [key, macro] of Object.entries(CAPABILITY_MACROS)) {
        lines.push('| ' + cell(key + ' / ' + macro) + ' | ' + report.environments.map((env) => labels[env.capabilityStates[key]]).join(' | ') + ' |');
    }
    lines.push('', '## 可重现来源', '',
        '以下是去 BOM、CRLF 归一为 LF 后的 UTF-8 文本 SHA256，避免 Git 换行策略导致误报；不是镜像或原始字节 SHA256。没有时间戳、绝对路径或设备信息。', '',
        '| 来源 | 文本 SHA256 |', '| --- | --- |');
    for (const [source, digest] of Object.entries(report.sourceHashes)) lines.push('| ' + cell(source) + ' | ' + digest + ' |');
    lines.push('', '每环境指纹沿用 Web 发布清单算法，包含解析后的构建定义；不能替代候选输入/源码/镜像哈希。', '',
        '| 环境 | Web capabilityFingerprint |', '| --- | --- |');
    for (const env of report.environments) lines.push('| ' + cell(env.environment) + ' | ' + env.capabilityFingerprint + ' |');
    return lines.join('\n') + '\n';
}

function generate(root = ROOT) {
    const texts = Object.fromEntries(SOURCES.map((name) => [name, fs.readFileSync(path.join(root, name), 'utf8')]));
    return render(collect(texts['platformio.ini'], Object.fromEntries(SOURCES.map((name) => [name, hash(texts[name])]))));
}

function checkReport(root = ROOT) {
    const file = path.join(root, OUTPUT);
    if (!fs.existsSync(file) || normalize(fs.readFileSync(file, 'utf8')) !== generate(root)) {
        throw new Error(OUTPUT + ' is missing/stale; run node scripts/generate-build-capabilities.js --write');
    }
    return true;
}

function writeReport(root = ROOT) {
    const file = path.join(root, OUTPUT);
    const expected = generate(root);
    if (fs.existsSync(file) && !normalize(fs.readFileSync(file, 'utf8')).startsWith(MARKER + '\n')) {
        throw new Error('Refusing to replace a document without the generated marker: ' + file);
    }
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, expected, 'utf8');
}

function main(argv = process.argv.slice(2)) {
    if (argv.length > 1 || (argv.length && !['--check', '--write', '--stdout'].includes(argv[0]))) {
        throw new Error('Usage: node scripts/generate-build-capabilities.js [--check|--write|--stdout]');
    }
    if (argv[0] === '--stdout') process.stdout.write(generate());
    else if (argv[0] === '--write') { writeReport(); console.log('Generated ' + OUTPUT); }
    else { checkReport(); console.log('Build capability documentation is current'); }
}

module.exports = { collect, render, normalize, hash, generate, checkReport, writeReport, OUTPUT, MARKER };
if (require.main === module) {
    try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
