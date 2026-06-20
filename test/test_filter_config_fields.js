/**
 * @file test_filter_config_fields.js
 * @brief filterConfigFields 单元测试（Node.js 直接运行）
 *
 * 运行方式: node test/test_filter_config_fields.js
 *
 * 覆盖范围：
 *  - 基本字段过滤（多余字段丢弃）
 *  - 缺失字段保留当前值
 *  - 嵌套对象递归过滤
 *  - 数组字段保留导入数据
 *  - 边界情况（null/undefined/空对象/类型不匹配）
 *  - 真实配置场景模拟（device.json、network.json、protocol.json）
 */

'use strict';

// ========== 模拟 window 环境并加载 utils.js ==========
const fs = require('fs');
const path = require('path');

// 创建最小 window polyfill
global.window = global;

// 加载 utils.js（会在 window 上注册 filterConfigFields）
const utilsPath = path.join(__dirname, '..', 'web-src', 'js', 'utils.js');
const utilsCode = fs.readFileSync(utilsPath, 'utf-8');
eval(utilsCode);

const filterConfigFields = window.filterConfigFields;

// ========== 测试框架 ==========
let totalTests = 0;
let passedTests = 0;
let failedTests = 0;

function assertEqual(actual, expected, msg) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a !== e) {
        throw new Error(`${msg}\n  期望: ${e}\n  实际: ${a}`);
    }
}

function assertDeepEqual(actual, expected, msg) {
    assertEqual(actual, expected, msg);
}

function runTest(name, fn) {
    totalTests++;
    try {
        fn();
        passedTests++;
        console.log(`  ✓ ${name}`);
    } catch (err) {
        failedTests++;
        console.log(`  ✗ ${name}`);
        console.log(`    ${err.message}`);
    }
}

// ========== 基本字段过滤 ==========

console.log('\n[filterConfigFields] 基本字段过滤');

runTest('多余字段被过滤', () => {
    const imported = { a: 1, b: 2, extra: 'should be removed' };
    const reference = { a: 0, b: 0 };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { a: 1, b: 2 }, '多余字段 extra 应被过滤');
});

runTest('多个多余字段全部被过滤', () => {
    const imported = { name: 'test', x: 1, y: 2, z: 3 };
    const reference = { name: '' };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { name: 'test' }, '只保留 name');
});

runTest('已知字段使用导入值', () => {
    const imported = { mode: 1, deviceName: 'MyDevice' };
    const reference = { mode: 0, deviceName: 'Default' };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { mode: 1, deviceName: 'MyDevice' }, '字段值应来自导入');
});

// ========== 缺失字段保留当前值 ==========

console.log('\n[filterConfigFields] 缺失字段保留当前值');

runTest('导入中缺失的字段保留 reference 值', () => {
    const imported = { a: 1 };
    const reference = { a: 0, b: 'keep-me', c: true };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { a: 1, b: 'keep-me', c: true }, '缺失字段 b、c 应保留');
});

runTest('导入空对象保留全部当前值', () => {
    const imported = {};
    const reference = { x: 10, y: 'hello' };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { x: 10, y: 'hello' }, '空导入应完全保留 reference');
});

// ========== 嵌套对象递归过滤 ==========

console.log('\n[filterConfigFields] 嵌套对象递归过滤');

runTest('嵌套对象多余字段被过滤', () => {
    const imported = {
        ethernet: { spiMosi: 11, spiMiso: 13, extraPin: 99 }
    };
    const reference = {
        ethernet: { spiMosi: 0, spiMiso: 0, spiSck: 12 }
    };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, {
        ethernet: { spiMosi: 11, spiMiso: 13, spiSck: 12 }
    }, '嵌套中 extraPin 被过滤，spiSck 保留');
});

runTest('多层嵌套递归过滤', () => {
    const imported = {
        level1: {
            level2: { a: 1, extra: 'no' },
            extra: 'no'
        }
    };
    const reference = {
        level1: {
            level2: { a: 0, b: 2 }
        }
    };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, {
        level1: { level2: { a: 1, b: 2 } }
    }, '多层嵌套正确过滤');
});

runTest('嵌套对象缺失保留当前值', () => {
    const imported = { top: 'val' };
    const reference = { top: '', nested: { inner: 1 } };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { top: 'val', nested: { inner: 1 } }, '缺失的嵌套对象保留');
});

// ========== 数组字段保留导入数据 ==========

console.log('\n[filterConfigFields] 数组字段保留导入数据');

runTest('数组字段使用导入的数组', () => {
    const imported = { networks: ['ssid1', 'ssid2'] };
    const reference = { networks: [] };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { networks: ['ssid1', 'ssid2'] }, '数组应使用导入值');
});

runTest('数组内对象不做字段过滤', () => {
    const imported = {
        tasks: [
            { slaveAddress: 1, functionCode: 3, customField: 'ok' }
        ]
    };
    const reference = { tasks: [] };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, {
        tasks: [{ slaveAddress: 1, functionCode: 3, customField: 'ok' }]
    }, '数组内对象整体保留，不过滤子字段');
});

runTest('导入值非数组但 reference 是数组时保留 reference', () => {
    const imported = { networks: 'invalid' };
    const reference = { networks: ['default'] };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { networks: ['default'] }, '类型不匹配时保留 reference 数组');
});

runTest('导入空数组覆盖 reference 数组', () => {
    const imported = { items: [] };
    const reference = { items: [1, 2, 3] };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { items: [] }, '空数组覆盖原有数组');
});

// ========== 边界情况 ==========

console.log('\n[filterConfigFields] 边界情况');

runTest('imported 为 null 返回 null', () => {
    const result = filterConfigFields(null, { a: 1 });
    assertEqual(result, null, 'null 导入应返回 null');
});

runTest('reference 为 null 返回 imported', () => {
    const imported = { a: 1 };
    const result = filterConfigFields(imported, null);
    assertDeepEqual(result, { a: 1 }, 'reference 为 null 时直接返回 imported');
});

runTest('imported 是数组但 reference 是对象 → 返回 reference', () => {
    const imported = [1, 2, 3];
    const reference = { a: 1 };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { a: 1 }, 'imported 类型不对时返回 reference');
});

runTest('reference 是数组时保留导入数组', () => {
    const imported = [4, 5, 6];
    const reference = [1, 2, 3];
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, [4, 5, 6], '两侧都是数组时使用导入值');
});

runTest('字段值类型不同但字段名匹配 → 使用导入值', () => {
    const imported = { mode: 'wifi' };  // string
    const reference = { mode: 0 };       // number
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { mode: 'wifi' }, '类型不同但字段名匹配，使用导入值');
});

runTest('导入值为 null 的已知字段被保留', () => {
    const imported = { name: null, age: 10 };
    const reference = { name: 'default', age: 0 };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { name: null, age: 10 }, 'null 值的已知字段照常导入');
});

runTest('reference 字段值为 null 但导入有对象 → 使用导入值', () => {
    const imported = { config: { x: 1 } };
    const reference = { config: null };
    const result = filterConfigFields(imported, reference);
    assertDeepEqual(result, { config: { x: 1 } }, 'reference 为 null 时取导入值');
});

runTest('两个空对象返回空对象', () => {
    const result = filterConfigFields({}, {});
    assertDeepEqual(result, {}, '两个空对象返回空对象');
});

// ========== 真实配置场景 ==========

console.log('\n[filterConfigFields] 真实配置场景模拟');

runTest('device.json: 过滤多余字段并保留缺失字段', () => {
    const currentDevice = {
        deviceId: 'FBE-AABBCC',
        productNumber: 100,
        userId: '1',
        deviceName: 'FastBee-Device',
        location: '',
        description: 'FastBee-Arduino',
        enableNTP: true,
        ntpServer1: 'http://iot.fastbee.cn/prod-api/iot/tool/ntp',
        ntpServer2: 'cn.pool.ntp.org',
        timezone: 'CST-8',
        syncInterval: 3600,
        logLevel: 'INFO',
        developerModeEnabled: true
    };
    const importedDevice = {
        deviceId: '',
        productNumber: 200,
        userId: '2',
        deviceName: 'Imported-Device',
        // location 和 description 缺失
        enableNTP: false,
        ntpServer1: 'https://new-server.com/ntp',
        ntpServer2: 'pool.ntp.org',
        timezone: 'UTC+0',
        syncInterval: 7200,
        logLevel: 'DEBUG',
        developerModeEnabled: false,
        // 多余字段
        firmwareVersion: '2.0.0',
        hardwareRevision: 'B'
    };
    const result = filterConfigFields(importedDevice, currentDevice);
    // 验证多余字段被过滤
    assertEqual('firmwareVersion' in result, false, 'firmwareVersion 应被过滤');
    assertEqual('hardwareRevision' in result, false, 'hardwareRevision 应被过滤');
    // 验证缺失字段保留
    assertEqual(result.location, '', 'location 应保留当前空值');
    assertEqual(result.description, 'FastBee-Arduino', 'description 应保留当前值');
    // 验证已知字段使用导入值
    assertEqual(result.productNumber, 200, 'productNumber 应为导入值');
    assertEqual(result.deviceName, 'Imported-Device', 'deviceName 应为导入值');
    assertEqual(result.logLevel, 'DEBUG', 'logLevel 应为导入值');
});

runTest('network.json: 嵌套对象 ethernet 过滤多余字段', () => {
    const currentNetwork = {
        mode: 0,
        networkType: 0,
        staSSID: 'my-wifi',
        staPassword: 'pass123',
        networks: [],
        ethernet: {
            spiMosi: 11,
            spiMiso: 13,
            spiSck: 12,
            csPin: 47,
            rstPin: 48,
            intPin: 14
        }
    };
    const importedNetwork = {
        mode: 1,
        networkType: 1,
        staSSID: 'new-wifi',
        staPassword: 'newpass',
        networks: [{ ssid: 'backup', password: '123' }],
        ethernet: {
            spiMosi: 23,
            spiMiso: 19,
            spiSck: 18,
            csPin: 5,
            rstPin: -1,
            intPin: 4,
            unknownPin: 99  // 多余
        },
        unknownSection: { x: 1 }  // 整个多余对象
    };
    const result = filterConfigFields(importedNetwork, currentNetwork);
    assertEqual('unknownSection' in result, false, 'unknownSection 应被过滤');
    assertEqual('unknownPin' in result.ethernet, false, 'ethernet.unknownPin 应被过滤');
    assertEqual(result.ethernet.spiMosi, 23, 'ethernet.spiMosi 应为导入值');
    assertEqual(result.ethernet.rstPin, -1, 'ethernet.rstPin 应为导入值');
    assertDeepEqual(result.networks, [{ ssid: 'backup', password: '123' }], 'networks 数组保留导入值');
});

runTest('protocol.json: MQTT 嵌套对象 + Modbus 任务数组', () => {
    const currentProtocol = {
        version: 2,
        mqtt: {
            enabled: true,
            host: 'broker.example.com',
            port: 1883,
            clientId: 'S&FBE-AA&100&1'
        },
        modbusRtu: {
            enabled: true,
            timeout: 1000,
            master: {
                responseTimeout: 500,
                maxRetries: 1,
                tasks: []
            }
        }
    };
    const importedProtocol = {
        version: 3,
        mqtt: {
            enabled: false,
            host: 'new-broker.io',
            port: 8883,
            clientId: '',
            scheme: 'mqtts',    // 多余字段
            useTLS: true        // 多余字段
        },
        modbusRtu: {
            enabled: false,
            timeout: 2000,
            master: {
                responseTimeout: 1000,
                maxRetries: 3,
                tasks: [
                    { slaveAddress: 1, functionCode: 3, startAddress: 0, quantity: 2 }
                ]
            },
            newFeature: 'extra'  // 多余
        },
        httpApi: { enabled: true }  // 整体多余
    };
    const result = filterConfigFields(importedProtocol, currentProtocol);
    assertEqual('httpApi' in result, false, 'httpApi 应被过滤');
    assertEqual('scheme' in result.mqtt, false, 'mqtt.scheme 应被过滤');
    assertEqual('useTLS' in result.mqtt, false, 'mqtt.useTLS 应被过滤');
    assertEqual('newFeature' in result.modbusRtu, false, 'modbusRtu.newFeature 应被过滤');
    assertEqual(result.mqtt.host, 'new-broker.io', 'mqtt.host 应为导入值');
    assertEqual(result.mqtt.port, 8883, 'mqtt.port 应为导入值');
    assertEqual(result.modbusRtu.master.tasks.length, 1, 'tasks 数组保留导入值');
    assertEqual(result.modbusRtu.master.tasks[0].slaveAddress, 1, 'tasks 内容完整保留');
});

runTest('导入文件完全匹配（无多余无缺失）→ 原样通过', () => {
    const config = { a: 1, b: 'str', c: true, d: [1, 2], e: { x: 10 } };
    const result = filterConfigFields(
        JSON.parse(JSON.stringify(config)),
        JSON.parse(JSON.stringify(config))
    );
    assertDeepEqual(result, config, '完全匹配时结果等于导入值');
});

// ========== 结果汇总 ==========

console.log('\n========================================');
console.log(`  测试完成: ${totalTests} 个`);
console.log(`  通过: ${passedTests} 个`);
if (failedTests > 0) {
    console.log(`  失败: ${failedTests} 个`);
    console.log('========================================\n');
    process.exit(1);
} else {
    console.log('  全部通过 ✓');
    console.log('========================================\n');
    process.exit(0);
}
