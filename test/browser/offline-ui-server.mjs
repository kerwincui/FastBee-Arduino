import { spawnSync } from 'node:child_process';
import { createReadStream, existsSync, readFileSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, join, normalize, resolve, sep } from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { genericPeripheralTypes, genericPeripherals } from './generic-peripheral-fixtures.mjs';

const browserDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(browserDir, '..', '..');
const wwwRoot = process.env.OFFLINE_UI_WEB_ROOT ? resolve(process.env.OFFLINE_UI_WEB_ROOT) : join(repositoryRoot, 'data', 'www');
// request-governor.js reserves localhost/127.0.0.1 for physical-device mDNS.
// Another loopback address keeps all browser API calls on this mock origin.
const host = process.env.OFFLINE_UI_HOST || '127.0.0.2';
const port = Number.parseInt(process.env.OFFLINE_UI_PORT || '41739', 10);
const require = createRequire(import.meta.url);
const { resolveWebBuildContext } = require('../../scripts/web-profile.js');
const profileFixture = JSON.parse(readFileSync(join(browserDir, 'offline-device-profiles.json'), 'utf8'));

const featureCatalog = [
  { key: 'webServer', name: 'Web 服务 (HTTP/API)', group: 'network', flashKB: 60, ramB: 36864, locked: true },
  { key: 'mdns', name: 'mDNS 发现 (.local)', group: 'network', flashKB: 5, ramB: 8192 },
  { key: 'lcd', name: 'OLED/LCD 显示屏', group: 'peripheral', flashKB: 20, ramB: 1200 },
  { key: 'neopixel', name: 'NeoPixel LED', group: 'peripheral', flashKB: 12, ramB: 212 },
  { key: 'sevenSegment', name: 'TM1637 七段数码管', group: 'peripheral', flashKB: 3, ramB: 32 },
  { key: 'ds18b20', name: 'DS18B20 温度传感器', group: 'peripheral', flashKB: 5, ramB: 100 },
  { key: 'modbus', name: 'Modbus 主站', group: 'protocol', flashKB: 20, ramB: 1024, requiresRestart: true },
  { key: 'ruleScript', name: '规则引擎（含命令脚本）', group: 'system', flashKB: 8, ramB: 2048, requiresRestart: true },
  { key: 'lcdGb2312', name: 'GB2312 中文字库', group: 'peripheral', flashKB: 248, ramB: 0, dependsOn: 'lcd' },
  { key: 'irRemote', name: '红外遥控', group: 'peripheral', flashKB: 8, ramB: 300, requiresRestart: true },
  { key: 'rfid', name: 'RFID MFRC522', group: 'peripheral', flashKB: 12, ramB: 200, requiresRestart: true },
  { key: 'ioExpander', name: '通用 IO 扩展器', group: 'peripheral', flashKB: 6, ramB: 1024, onDemand: true },
  { key: 'i2cSensors', name: 'I2C 传感器采集', group: 'peripheral', flashKB: 15, ramB: 500, requiresRestart: true },
  { key: 'rfLearn', name: '433MHz RF 学习', group: 'peripheral', flashKB: 4, ramB: 300 },
  { key: 'ds1302', name: 'DS1302 RTC', group: 'peripheral', flashKB: 2, ramB: 100 },
  { key: 'lcd1602', name: 'LCD1602 字符屏', group: 'peripheral', flashKB: 3, ramB: 100 },
  { key: 'commandScript', name: null, group: 'system', flashKB: 3, ramB: 500, dependsOn: 'ruleScript', requiresRestart: true },
  { key: 'mqtts', name: 'MQTTS (加密传输)', group: 'protocol', flashKB: 0, ramB: 43008 },
  { key: 'deviceControl', name: '设备控制面板', group: 'system', flashKB: 27, ramB: 0 },
  { key: 'ota', name: 'OTA 固件升级', group: 'system', flashKB: 10, ramB: 1024 },
  { key: 'otaFs', name: null, group: 'system', flashKB: 3, ramB: 500, dependsOn: 'ota' },
  { key: 'logViewer', name: '设备日志', group: 'system', flashKB: 4, ramB: 500 },
  { key: 'fileLogging', name: null, group: 'system', flashKB: 3, ramB: 500, dependsOn: 'logViewer' },
  { key: 'userAdmin', name: '多用户管理', group: 'system', flashKB: 5, ramB: 500 },
  { key: 'ble', name: 'BLE Bluetooth', group: 'network', flashKB: 80, ramB: 10240 },
  { key: 'ethernet', name: '以太网 W5500', group: 'network', flashKB: 8, ramB: 2048 },
  { key: 'cellular', name: '4G Cellular', group: 'network', flashKB: 15, ramB: 4096 },
  { key: 'remoteResources', name: '远程资源', group: 'protocol', flashKB: 34, ramB: 12288 },
  { key: 'screenPanel', name: '设备屏幕', group: 'system', flashKB: 180, ramB: 65536, requiresRestart: true },
];

const targetProfiles = Object.fromEntries(Object.entries(profileFixture.profiles).map(([environment, fixture]) => {
  const buildContext = resolveWebBuildContext({ environment });
  const enabledFeatures = new Set(fixture.enabledFeatures);
  const capabilities = { ...buildContext.compiledCapabilities };
  const features = featureCatalog.map((feature) => {
    const capabilityKey = feature.key === 'mqtts' ? 'mqtt' : feature.key;
    const compiled = capabilities[capabilityKey] === true;
    const userEnabled = compiled && enabledFeatures.has(feature.key);
    return {
      ...feature,
      compiled,
      userEnabled,
      active: userEnabled,
      budgetPassed: true,
      bootEnabled: userEnabled,
      pendingRestart: false,
    };
  });
  const featureStates = Object.fromEntries(features.map((feature) => [
    feature.key,
    feature.active ? 'active' : 'inactive',
  ]));
  featureStates.mqtt = fixture.protocol.mqttEnabled ? 'active' : 'inactive';
  featureStates.periphExec = capabilities.periphExec === true ? 'active' : 'inactive';
  featureStates.gpio = capabilities.gpio === true ? 'active' : 'inactive';
  const activeFeatures = features.filter((feature) => feature.active);
  return [environment, {
    ...fixture,
    environment,
    capabilities,
    features,
    featureStates,
    summary: {
      totalFlashKB: features.reduce((total, feature) => total + Number(feature.flashKB || 0), 0),
      activeFlashKB: activeFeatures.reduce((total, feature) => total + Number(feature.flashKB || 0), 0),
      activeRamB: activeFeatures.reduce((total, feature) => total + Number(feature.ramB || 0), 0),
      free8KB: fixture.psramBytes > 0 ? 96 : 40,
      largest8KB: fixture.psramBytes > 0 ? 64 : 24,
    },
  }];
}));

if (process.env.OFFLINE_UI_SKIP_BUILD !== '1') {
  const build = spawnSync(
    process.execPath,
    [join(repositoryRoot, 'scripts', 'build-web-assets.js'), '--env=esp32s3-F16R8'],
    { cwd: repositoryRoot, env: process.env, stdio: 'inherit' }
  );
  if (build.status !== 0) process.exit(build.status || 1);
}

const featureStates = {
  webServer: 'active',
  mdns: 'active',
  mqtt: 'inactive',
  modbus: 'inactive',
  periphExec: 'active',
  ioExpander: 'active',
  i2cSensors: 'active',
  ruleScript: 'inactive',
  deviceControl: 'inactive',
  logViewer: 'inactive',
  userAdmin: 'inactive',
  remoteResources: 'active',
  screenPanel: 'active',
};

const features = [
  { key: 'webServer', name: 'Web 服务', group: 'network', compiled: true, userEnabled: true, active: true, locked: true, flashKB: 42, ramB: 8192 },
  { key: 'mdns', name: 'mDNS 发现', group: 'network', compiled: true, userEnabled: true, active: true, flashKB: 8, ramB: 1024 },
  { key: 'mqtt', name: 'MQTT', group: 'protocol', compiled: true, userEnabled: false, active: false, flashKB: 38, ramB: 6144 },
  { key: 'modbus', name: 'Modbus RTU', group: 'protocol', compiled: true, userEnabled: false, active: false, flashKB: 22, ramB: 2048 },
  { key: 'periphExec', name: '外设执行', group: 'peripheral', compiled: true, userEnabled: true, active: true, flashKB: 31, ramB: 4096 },
  { key: 'gpio', name: 'GPIO', group: 'peripheral', compiled: true, userEnabled: true, active: true, flashKB: 4, ramB: 256 },
  { key: 'ioExpander', name: '通用 IO 扩展器', group: 'peripheral', compiled: true, userEnabled: true, active: true, budgetPassed: true, bootEnabled: true, requiresRestart: true, pendingRestart: false, onDemand: true, flashKB: 6, ramB: 1024 },
  { key: 'i2cSensors', name: 'I2C 传感器采集', group: 'peripheral', compiled: true, userEnabled: true, active: true, budgetPassed: true, flashKB: 18, ramB: 2048 },
  { key: 'remoteResources', name: '远程资源', group: 'protocol', compiled: true, userEnabled: true, active: true, budgetPassed: true, flashKB: 34, ramB: 12288 },
  { key: 'screenPanel', name: '设备屏幕', group: 'system', compiled: true, userEnabled: true, active: true, budgetPassed: true, requiresRestart: true, flashKB: 180, ramB: 65536 },
];

const peripheral = {
  id: 'offline_gpio_1',
  name: '离线测试 GPIO',
  type: 12,
  typeName: '数字输出',
  enabled: true,
  implemented: true,
  pins: [1],
  pinCount: 1,
  params: {},
  status: 3,
};

const displayPeripheral = {
  id: 'display_main',
  name: '4.0 英寸触摸屏',
  type: 56,
  typeName: '显示屏',
  enabled: true,
  implemented: true,
  driver: 'st7701',
  width: 480,
  height: 480,
  rotation: 0,
  touchEnabled: true,
  touchDriver: 'gt911',
  touchSda: 19,
  touchScl: 45,
  touchAddress: 0x5d,
  params: {
    driver: 'st7701', width: 480, height: 480, rotation: 0,
    touchEnabled: true, touchDriver: 'gt911', touchSda: 19, touchScl: 45, touchAddress: 0x5d,
  },
  status: 3,
};

const peripherals = [peripheral, displayPeripheral, ...structuredClone(genericPeripherals)];
const executionRules = [];
const ruleScripts = [
  { id: 'rule-1', name: '规则一', enabled: true, triggerType: 0, protocolType: 0, triggerCount: 0 },
  { id: 'rule-2', name: '规则二', enabled: false, triggerType: 1, protocolType: 1, triggerCount: 0 },
  { id: 'rule-3', name: '规则三', enabled: true, triggerType: 1, protocolType: 0, triggerCount: 0 },
];

const initialScreenPanelConfig = {
  schemaVersion: 1,
  displayRef: 'display_main',
  settings: {
    brightnessPercent: 70,
    idleSleepSeconds: 120,
    homePageId: 'home',
    themeId: 'dark',
    background: { presetId: 'solid' },
    allowSystemRestart: false,
  },
  pages: [{
    id: 'home', title: '首页', layoutId: 'mixed-grid', layoutMode: 'template',
    logicalWidth: 480, logicalHeight: 480, rotation: 0, cards: [],
  }],
};

const initialRemoteResources = {
  remoteChannels: [{
    id: 'living_light', label: '客厅灯', enabled: true, type: 'switch',
    commandTopic: 'home/living/light/set', stateTopic: 'home/living/light/state',
    onPayload: 'ON', offPayload: 'OFF', actionPayload: '', onValue: 'ON', offValue: 'OFF',
    valuePath: '', unit: '', staleAfterSeconds: 120, timeoutSeconds: 10,
  }, {
    id: 'target_temperature', label: '目标温度', enabled: true, type: 'number',
    commandTopic: 'home/hvac/target/set', stateTopic: 'home/hvac/target/state',
    onPayload: '', offPayload: '', actionPayload: '', onValue: '', offValue: '',
    valuePath: '', unit: '°C', staleAfterSeconds: 120, timeoutSeconds: 10,
  }],
  weatherSources: [{
    id: 'main', label: '本地天气', enabled: true, provider: 'seniverse',
    url: 'https://api.seniverse.com/v3/weather/now.json', apiKey: 'offline-secret',
    apiKeyParam: '', authHeader: 'Bearer offline-secret', location: 'shanghai', intervalSeconds: 1800,
    staleAfterSeconds: 7200, forecastEnabled: true,
    forecastUrl: 'https://api.seniverse.com/v3/weather/daily.json', forecastIntervalSeconds: 3600,
    fields: { city: '', temperature: '', condition: '', description: '', humidity: '', forecast: '',
      forecastDate: '', forecastLow: '', forecastHigh: '', forecastCondition: '' },
    codeMap: {},
  }, {
    id: 'backup', label: '备用天气', enabled: false, provider: 'custom',
    url: 'https://weather.example.test/now', apiKey: 'backup-secret',
    apiKeyParam: 'apikey', authHeader: 'Bearer backup-secret', location: 'shanghai', intervalSeconds: 1800,
    staleAfterSeconds: 7200, forecastEnabled: false,
    forecastUrl: 'https://weather.example.test/forecast', forecastIntervalSeconds: 3600,
    fields: { city: 'city', temperature: 'temperature', condition: 'condition', description: 'description', humidity: 'humidity', forecast: 'forecast',
      forecastDate: 'date', forecastLow: 'minimum', forecastHigh: 'maximum', forecastCondition: 'code' },
    codeMap: {},
  }],
};

let weatherRevision = 1;
let screenWeather = { id: 'main', enabled: true, location: 'shanghai', intervalSeconds: 1800, apiKey: 'offline-secret' };
function screenWeatherPublic() {
  const { apiKey, ...data } = screenWeather;
  return { ...data, revision: weatherRevision, hasApiKey: !!apiKey, migration: { required: false }, status: { preview: { city: '上海', temperature: 26.5, description: '晴' } } };
}
let screenPanelRevision = 7;
let screenPanelConfig = structuredClone(initialScreenPanelConfig);
let historyConfig = { revision: 0, enabled: true, sourceAllowlist: ['modbus.boiler.temperature', 'legacy.missing'], maxSources: 16, writesPaused: false };
let historyStatus = { persisted: true, sourceCount: 2 };
let remoteResources = structuredClone(initialRemoteResources);
let requestCounts = {};
let weatherTestLastRequest = {};

function resetSmartPanelState() {
  weatherRevision = 1;
  screenWeather = { id: 'main', enabled: true, location: 'shanghai', intervalSeconds: 1800, apiKey: 'offline-secret' };
  screenPanelRevision = 7;
  screenPanelConfig = structuredClone(initialScreenPanelConfig);
  historyConfig = { revision: 0, enabled: true, sourceAllowlist: ['modbus.boiler.temperature', 'legacy.missing'], maxSources: 16, writesPaused: false };
  historyStatus = { persisted: true, sourceCount: 2 };
  remoteResources = structuredClone(initialRemoteResources);
  requestCounts = {};
  weatherTestLastRequest = {};
  const displayIndex = peripherals.findIndex((item) => item.id === displayPeripheral.id);
  if (displayIndex >= 0) peripherals[displayIndex] = structuredClone(displayPeripheral);
}

function maskedRemoteResources() {
  const data = structuredClone(remoteResources);
  for (const source of data.weatherSources) {
    if (source.apiKey) source.apiKey = '********';
    if (source.authHeader) source.authHeader = '********';
  }
  return { ...data, capabilities: {
    maxRemoteChannels: 8, maxWeatherSources: 2, maxForecastDays: 3,
    weatherTestRead: true, weatherTestMinIntervalSeconds: 300,
  } };
}

function responseFor(pathname, searchParams = new URLSearchParams(), targetProfile = null) {
  switch (pathname) {
    case '/api/health':
    case '/api/system/health':
      return {
        success: true,
        data: {
          status: 'healthy',
          uptime: 3_600_000,
          freeHeap: targetProfile?.psramBytes > 0 ? 98_304 : 49_152,
          memory: {
            heapFree: targetProfile?.psramBytes > 0 ? 98_304 : 49_152,
            heapTotal: 327_680,
            largestFreeBlock: 32_768,
            psramFree: targetProfile?.psramBytes ? Math.floor(targetProfile.psramBytes * 0.75) : 0,
            psramTotal: targetProfile?.psramBytes || 0,
          },
        },
      };
    case '/api/auth/session':
      return { success: true, data: { sessionValid: true, username: 'Offline UI' } };
    case '/api/auth/login':
      return { success: true, username: 'Offline UI' };
    case '/api/system/capabilities':
      if (targetProfile) {
        return {
          success: true,
          data: {
            ...targetProfile.capabilities,
            healthMonitor: true,
            logger: targetProfile.capabilities.logViewer === true,
          },
        };
      }
      return {
        success: true,
        data: {
          webServer: true,
          auth: true,
          healthMonitor: true,
          mdns: true,
          mqtt: true,
          modbus: false,
          periphExec: true,
          gpio: true,
          ioExpander: true,
          i2cSensors: true,
          ota: true,
          configTransfer: true,
          ruleScript: false,
          deviceControl: false,
          logger: false,
          logViewer: false,
          fileLogging: false,
          userAdmin: false,
          remoteResources: true,
          screenPanel: true,
          ethernet: false,
          cellular: false,
          i18n: false,
        },
      };
    case '/api/system/status':
      return {
        success: true,
        data: {
          uptime: 3_600_000,
          freeHeap: 49_152,
          heapTotal: 327_680,
          memoryPercent: 85,
          networkConnected: false,
          ipAddress: '',
          features: targetProfile?.featureStates || featureStates,
        },
      };
    case '/api/system/info': {
      const heapFree = targetProfile?.psramBytes > 0 ? 98_304 : 49_152;
      const flashSize = targetProfile?.flashBytes || 16 * 1024 * 1024;
      const psramSize = targetProfile?.psramBytes || 0;
      const chipModel = targetProfile
        ? `${targetProfile.chipModel} (${targetProfile.environment} offline profile)`
        : 'ESP32-S3 (offline UI)';
      const data = {
        chipModel,
        cpuFreqMHz: 240,
        sdkVersion: 'offline',
        firmwareVersion: 'offline-ui-gate',
        uptime: { milliseconds: 3_600_000, formatted: '1h 0m' },
        device: { chipModel, cpuFreqMHz: 240, sdkVersion: 'offline' },
        network: { connected: false, ssid: '', ipAddress: '' },
        memory: { heapFree, heapTotal: 327_680, heapUsed: 327_680 - heapFree, usagePercent: Math.round((1 - heapFree / 327_680) * 100) },
        flash: { total: flashSize, used: 1024 * 1024, free: flashSize - 1024 * 1024, usagePercent: Math.round((1024 * 1024 / flashSize) * 1000) / 10 },
        filesystem: { total: 1024 * 1024, used: 278_528, free: 770_048, usagePercent: 26.6 },
        heapFree,
        freeHeap: heapFree,
        heapSize: 327_680,
        heapTotal: 327_680,
        psramFree: psramSize ? Math.floor(psramSize * 0.75) : 0,
        psramSize,
        flashSize,
      };
      return { success: true, ...data, data };
    }
    case '/api/network/status':
      return {
        success: true,
        data: {
          type: 0,
          mode: 'STA',
          connected: false,
          status: 'disconnected',
          ssid: '',
          staSSID: '',
          ipAddress: '',
          macAddress: '02:00:00:00:00:01',
          rssi: 0,
          uptime: 0,
        },
      };
    case '/api/network/config':
      return {
        success: true,
        data: {
          network: { type: 0, mode: 2 },
          sta: { ssid: '', security: 'wpa2', ipConfigType: 0 },
          ap: { ssid: 'FastBee-Offline', security: 'wpa2' },
          ethernet: { enabled: false },
          cellular: { enabled: false },
          mdns: { hostname: 'fastbee-offline' },
        },
      };
    case '/api/device/config':
      return {
        success: true,
        data: {
          deviceId: 'OFFLINE-UI',
          deviceName: '离线界面测试',
          productId: '',
          developerModeEnabled: true,
          cacheDuration: 0,
          timezone: 'Asia/Shanghai',
          ntpEnabled: false,
        },
      };
    case '/api/device/time':
      return { success: true, data: { timestamp: 1_788_739_200, timezone: 'Asia/Shanghai', synchronized: false } };
    case '/api/peripherals':
    case '/api/peripherals/':
      if (searchParams.get('id')) {
        return { success: true, data: peripherals.find((item) => item.id === searchParams.get('id')) || null };
      }
      return {
        success: true,
        data: peripherals,
        total: peripherals.length,
        page: 1,
        pageSize: 10,
        profile: { maxItems: 16, used: peripherals.length },
      };
    case '/api/peripherals/types':
      return {
        success: true,
        data: genericPeripheralTypes,
        hardware: {
          chip: targetProfile?.chipModel || 'ESP32-S3',
          inputPins: [1, 2, 4, 5],
          outputPins: [1, 2, 4, 5],
          adcPins: [1, 2],
          dacPins: [],
          touchPins: [],
        },
      };
    case '/api/periph-exec':
      if (searchParams.get('id')) {
        return { success: true, data: executionRules.find((item) => item.id === searchParams.get('id')) || null };
      }
      return { success: true, data: executionRules, total: executionRules.length, page: 1, pageSize: 10 };
    case '/api/periph-exec/results':
      return { success: true, data: [] };
    case '/api/periph-exec/history/config':
      return { success: true, data: { config: structuredClone(historyConfig), status: structuredClone(historyStatus) } };
    case '/api/periph-exec/history': {
      const sourceRef = searchParams.get('sourceRef') || 'modbus.boiler.temperature';
      return {
        success: true,
        data: {
          sourceRef,
          points: Array.from({ length: 10 }, (_, index) => ({ timestamp: 1_788_739_200 + index * 60, value: 21.5 + index * 0.35 })),
        },
      };
    }
    case '/api/screen-panel/weather':
      return { success: true, data: screenWeatherPublic() };
    case '/api/screen-panel/weather/refresh':
      return { success: true, data: { pending: false, preview: { city: '上海', temperature: 26.5, description: '晴' } } };
    case '/api/screen-panel/config':
      return {
        success: true,
        data: {
          revision: screenPanelRevision,
          config: structuredClone(screenPanelConfig),
          capabilities: {
            maxPages: 8, maxCards: 24, maxCardsPerPage: 12,
            displays: [structuredClone(displayPeripheral)],
          },
          application: { status: 'running', appliedRevision: screenPanelRevision, pendingRestart: false },
        },
      };
    case '/api/screen-panel/choices': {
      const items = [
        { id: 'weather.main', sourceRef: 'weather.main', label: '上海天气', kind: 'weather', type: 'weather', unit: '°C', group: 'remote', available: true },
        { id: 'modbus.boiler.temperature', sourceRef: 'modbus.boiler.temperature', label: '锅炉温度', kind: 'data', type: 'number', unit: '°C', group: 'local', available: true },
        { id: 'can.motor.rpm', sourceRef: 'can.motor.rpm', label: '电机转速', kind: 'data', type: 'number', unit: 'rpm', group: 'local', available: true },
        { id: 'i2c.room.humidity', sourceRef: 'i2c.room.humidity', label: '室内湿度', kind: 'data', type: 'number', unit: '%', group: 'local', available: true },
        { id: 'motor.1.12345678', controlRef: 'motor.1.12345678', label: '传送电机', kind: 'control', type: 'motor', available: true },
        { id: 'rule.living-light', controlRef: 'rule.living-light', label: '客厅灯', kind: 'control', type: 'switch', group: 'rule', available: true },
        { id: 'rule.away-scene', controlRef: 'rule.away-scene', label: '离家场景', kind: 'control', type: 'action', group: 'rule', available: true, requiresConfirmation: true },
        { id: 'system.restart', label: '重启设备', kind: 'system', type: 'action', systemActionId: 'restart', available: true, requiresConfirmation: true },
      ];
      return {
        success: true,
        data: {
          items, total: items.length, page: 1, pageSize: 50, hasMore: false,
          capabilities: { history: { supported: true, maxSources: 16 } },
        },
      };
    }
    case '/api/screen-panel/state':
      return {
        success: true,
        data: {
          revision: screenPanelRevision,
          appliedRevision: screenPanelRevision,
          application: { status: 'running', appliedRevision: screenPanelRevision, pendingRestart: false },
          items: {
            'weather.main': {
              value: 26.5, temperature: 26.5, unit: '°C', condition: 'sunny', city: '上海', quality: 'good',
              forecast: [
                { date: '09-17', condition: 'sunny', low: 21, high: 29 },
                { date: '09-18', condition: 'cloudy', low: 22, high: 28 },
                { date: '09-19', condition: 'rain', low: 20, high: 25 },
              ],
            },
            'modbus.boiler.temperature': { value: 24.8, unit: '°C', quality: 'good' },
            'can.motor.rpm': { value: 1460, unit: 'rpm', quality: 'good' },
            'i2c.room.humidity': { value: 58.2, unit: '%', quality: 'good' },
            'rule.living-light': { state: true, text: '开启', quality: 'good', pending: false },
            'rule.away-scene': { state: false, text: '待命', quality: 'good', pending: false },
          },
        },
      };
    case '/api/remote-resources/config':
      return { success: true, data: maskedRemoteResources() };
    case '/api/remote-resources/weather-test': {
      const id = String(searchParams.get('id') || '');
      if (!remoteResources.weatherSources.some((source) => source.id === id && source.enabled !== false)) {
        return { success: false, error: 'Weather source unavailable', code: 404 };
      }
      return {
        success: true,
        data: {
          sourceId: id,
          label: '本地天气',
          status: 'ready',
          pending: false,
          updatedMs: 1_234_000,
          forecastUpdatedMs: 1_233_000,
          retryAfterMs: 299_000,
          preview: {
            temperature: 26.5,
            unit: '°C',
            condition: 'sunny',
            city: '上海',
            description: '晴',
            humidity: 58,
            forecast: [
              { date: '09-20', condition: 'sunny', low: 21, high: 29 },
              { date: '09-21', condition: 'cloudy', low: 22, high: 28 },
              { date: '09-22', condition: 'rain', low: 20, high: 25 },
            ],
          },
          forecastError: '第三日之后的预报未请求',
        },
      };
    }
    case '/api/rule-script':
      return { success: true, data: ruleScripts };
    case '/api/protocol/config':
      return {
        success: true,
        data: {
          mqtt: { enabled: targetProfile?.protocol.mqttEnabled || false, port: 1883, ssl: false },
          modbus: { enabled: targetProfile?.protocol.modbusEnabled || false, devices: [], tasks: [] },
          modbusRtu: { enabled: targetProfile?.protocol.modbusEnabled || false, mode: 'master', master: { devices: [], tasks: [] } },
          periphExec: { enabled: true },
        },
      };
    case '/api/mqtt/status':
      return { success: true, data: { enabled: false, connected: false, state: 'disabled', reason: 'not_configured' } };
    case '/api/features/status':
      if (targetProfile) {
        return {
          success: true,
          data: {
            features: targetProfile.features,
            summary: targetProfile.summary,
          },
        };
      }
      return {
        success: true,
        data: {
          features,
          summary: {
            activeFlashKB: 85,
            activeRamB: 13_568,
            free8KB: 48,
            largest8KB: 32,
          },
        },
      };
    case '/api/test/offline-profile':
      return targetProfile
        ? {
            success: true,
            data: {
              environment: targetProfile.environment,
              chipModel: targetProfile.chipModel,
              flashBytes: targetProfile.flashBytes,
              psramBytes: targetProfile.psramBytes,
              memoryProfile: targetProfile.memoryProfile,
              evidence: targetProfile.evidence,
              enabledFeatures: targetProfile.enabledFeatures,
              protocol: targetProfile.protocol,
            },
          }
        : { success: true, data: { environment: 'legacy-offline-ui' } };
    case '/api/test/smart-panel-state':
      return {
        success: true,
        data: {
          screenPanelRevision,
          screenWeather: screenWeatherPublic(),
          screenPanelConfig: structuredClone(screenPanelConfig),
          historyConfig: structuredClone(historyConfig),
          remoteResources: maskedRemoteResources(),
          weatherSecrets: Object.fromEntries(remoteResources.weatherSources.map((source) => [source.id, {
            hasApiKey: Boolean(source.apiKey), hasAuthHeader: Boolean(source.authHeader),
          }])),
          requestCounts: structuredClone(requestCounts),
        },
      };
    default:
      return { success: true, data: {} };
  }
}

function sendJson(response, status, body, method = 'GET') {
  const encoded = Buffer.from(JSON.stringify(body));
  response.writeHead(status, {
    'Cache-Control': 'no-store',
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': encoded.length,
  });
  response.end(method === 'HEAD' ? undefined : encoded);
}

async function readPayload(request) {
  const chunks = [];
  for await (const chunk of request) chunks.push(chunk);
  if (chunks.length === 0) return {};
  const content = Buffer.concat(chunks).toString('utf8');
  const type = String(request.headers['content-type'] || '').toLowerCase();
  try {
    if (type.includes('application/json')) return JSON.parse(content);
    return Object.fromEntries(new URLSearchParams(content));
  } catch {
    return {};
  }
}

function booleanValue(value, fallback = false) {
  if (typeof value === 'boolean') return value;
  if (value === undefined || value === null || value === '') return fallback;
  return value === 'true' || value === '1' || value === 1;
}

function normalizePeripheral(body) {
  const params = { ...(body.params || {}) };
  for (const key of ['driver', 'driverName', 'busPeripheralId', 'address', 'i2cAddress', 'sampleInterval', 'channel', 'mode', 'shuntResistance', 'maxCurrent', 'conversionTimeUs', 'averaging', 'ioSource', 'providerId', 'initialState', 'invertLogic', 'inputMode', 'debounceMs', 'width', 'height', 'rotation', 'touchEnabled', 'touchDriver', 'touchSda', 'touchScl', 'touchAddress', 'touchInterrupt', 'touchReset']) {
    if (body[key] !== undefined) params[key] = body[key];
  }
  // The firmware stores MPU mode, not a default sensor measurement channel.
  if (Number(body.type) === 38) delete params.channel;
  if (Number(body.type) === 56) {
    params.backend = body.displayBackend || params.backend || 'spi_tft';
    params.driver = body.displayDriver || params.driver || 'st7789';
    for (const key of ['touchBackend', 'colorDepth', 'bufferLines', 'brightness', 'backlightChannel', 'backlightActiveHigh', 'spiFrequency', 'i2cFrequency',
      'rgbInitProfile', 'rgbFrequency', 'hsyncFrontPorch', 'hsyncPulseWidth', 'hsyncBackPorch', 'vsyncFrontPorch', 'vsyncPulseWidth', 'vsyncBackPorch',
      'hsyncIdleLow', 'vsyncIdleLow', 'pclkActiveNeg', 'controlBackend', 'controlAddress', 'controlResetMask', 'controlBacklightMask', 'controlEnableMask',
      'controlBuzzerMask', 'controlInactiveMask', 'controlTouchIntMask']) {
      if (body[key] !== undefined) params[key] = body[key];
    }
    params.pins = { ...(params.pins || {}) };
    for (const key of ['mosi', 'sclk', 'cs', 'dc', 'rst', 'backlight', 'touchSda', 'touchScl', 'touchInt', 'touchRst', 'rgbDe', 'rgbVsync', 'rgbHsync', 'rgbPclk', 'controlSda', 'controlScl']) {
      if (body[key] !== undefined) params.pins[key] = Number(body[key]);
    }
    if (body.rgbData0 !== undefined) params.pins.rgbDataPins = Array.from({ length: 16 }, (_, i) => Number(body['rgbData' + i]));
  }
  return {
    ...body,
    id: String(body.id || ''),
    name: String(body.name || ''),
    type: Number(body.type || 0),
    typeName: Number(body.type) === 12 ? '数字输出' : Number(body.type) === 56 ? '显示屏' : '外设',
    enabled: booleanValue(body.enabled, false),
    implemented: true,
    featureActive: true,
    pins: String(body.pins || '').split(',').filter(Boolean).map(Number),
    pinCount: String(body.pins || '').split(',').filter(Boolean).length,
    params,
    status: 3,
  };
}

function mimeType(pathname) {
  const extension = extname(pathname).toLowerCase();
  return {
    '.css': 'text/css; charset=utf-8',
    '.html': 'text/html; charset=utf-8',
    '.ico': 'image/x-icon',
    '.js': 'text/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.png': 'image/png',
  }[extension] || 'application/octet-stream';
}

function resolveStaticFile(pathname) {
  let decoded;
  try {
    decoded = decodeURIComponent(pathname);
  } catch {
    return null;
  }
  const logicalPath = decoded === '/' ? '/index.html' : decoded;
  const relativePath = normalize(logicalPath).replace(/^([/\\])+/, '');
  const candidate = resolve(wwwRoot, relativePath);
  if (candidate !== wwwRoot && !candidate.startsWith(`${wwwRoot}${sep}`)) return null;
  if (existsSync(candidate) && statSync(candidate).isFile()) {
    return { file: candidate, logicalPath, gzip: false };
  }
  const gzipCandidate = `${candidate}.gz`;
  if (existsSync(gzipCandidate) && statSync(gzipCandidate).isFile()) {
    return { file: gzipCandidate, logicalPath, gzip: true };
  }
  return null;
}

const server = createServer(async (request, response) => {
  const method = request.method || 'GET';
  const url = new URL(request.url || '/', `http://${host}:${port}`);
  const requestedProfile = String(request.headers['x-fastbee-offline-profile'] || '').trim();
  const targetProfile = requestedProfile ? targetProfiles[requestedProfile] : null;

  if (url.pathname.startsWith('/api/')) {
    const requestKey = `${method} ${url.pathname}`;
    requestCounts[requestKey] = (requestCounts[requestKey] || 0) + 1;
  }

  if (requestedProfile && !targetProfile) {
    sendJson(response, 400, { success: false, message: `Unknown offline profile: ${requestedProfile}` }, method);
    return;
  }

  if (url.pathname === '/api/events') {
    response.writeHead(200, { 'Cache-Control': 'no-store', 'Content-Type': 'text/event-stream', Connection: 'keep-alive' });
    response.write('event: heartbeat\ndata: {"uptime":3600000}\n\n');
    request.on('close', () => response.end());
    return;
  }

  if (url.pathname === '/api/batch' && method === 'POST') {
    const body = await readPayload(request);
    const requests = Array.isArray(body.requests) ? body.requests : [];
    const results = requests.map((item) => {
      if (String(item?.method || 'GET').toUpperCase() !== 'GET') {
        requestCounts['POST /api/batch-write-attempt'] = (requestCounts['POST /api/batch-write-attempt'] || 0) + 1;
        return { success: false, error: 'Batch only accepts read requests', code: 400 };
      }
      const itemUrl = new URL(String(item?.url || '/'), `http://${host}:${port}`);
      return responseFor(itemUrl.pathname, itemUrl.searchParams, targetProfile);
    });
    sendJson(response, 200, { success: true, results }, method);
    return;
  }

  if (url.pathname === '/api/test/smart-panel-reset' && method === 'POST') {
    resetSmartPanelState();
    sendJson(response, 200, { success: true }, method);
    return;
  }

  if (url.pathname === '/api/test/smart-panel-conflict' && method === 'POST') {
    screenPanelRevision += 1;
    sendJson(response, 200, { success: true, data: { revision: screenPanelRevision } }, method);
    return;
  }

  if (url.pathname === '/api/screen-panel/weather' && method === 'POST') {
    const body = await readPayload(request);
    if (Number(body.expectedRevision) !== weatherRevision) {
      sendJson(response, 409, { success: false, error: '天气配置已被修改', data: { currentRevision: weatherRevision } }, method); return;
    }
    screenWeather = { ...screenWeather, enabled: body.enabled === true, location: body.location,
      intervalSeconds: Number(body.intervalSeconds), apiKey: body.apiKey || screenWeather.apiKey };
    weatherRevision++;
    sendJson(response, 200, { success: true, data: screenWeatherPublic() }, method); return;
  }
  if (url.pathname === '/api/screen-panel/weather/refresh' && method === 'POST') {
    const body = await readPayload(request);
    if (!screenWeather.enabled || body.id !== screenWeather.id) {
      sendJson(response, 404, { success: false, error: '天气来源不可用' }, method); return;
    }
    if (weatherTestLastRequest[body.id]) {
      sendJson(response, 429, { success: false, error: '300 秒后可再次更新' }, method); return;
    }
    weatherTestLastRequest[body.id] = true;
    sendJson(response, 200, { success: true, data: { accepted: true, pending: true } }, method); return;
  }

  if (url.pathname === '/api/screen-panel/config' && method === 'POST') {
    const body = await readPayload(request);
    if (Number(body.expectedRevision) !== screenPanelRevision) {
      sendJson(response, 409, {
        success: false,
        error: 'Revision conflict',
        message: 'Revision conflict',
        data: { currentRevision: screenPanelRevision },
      }, method);
      return;
    }
    screenPanelConfig = structuredClone(body.config || initialScreenPanelConfig);
    screenPanelRevision += 1;
    sendJson(response, 200, {
      success: true,
      data: {
        revision: screenPanelRevision,
        config: structuredClone(screenPanelConfig),
        application: { status: 'running', appliedRevision: screenPanelRevision, pendingRestart: false },
      },
    }, method);
    return;
  }

  if (url.pathname === '/api/periph-exec/history/config' && method === 'POST') {
    const body = await readPayload(request);
    if (!Number.isInteger(body.expectedRevision)) {
      sendJson(response, 400, { success: false, message: 'expectedRevision is required' }, method);
      return;
    }
    if (body.expectedRevision !== historyConfig.revision) {
      sendJson(response, 409, { success: false, message: 'History configuration revision conflict' }, method);
      return;
    }
    historyConfig = {
      ...historyConfig,
      revision: historyConfig.revision + 1,
      enabled: body.enabled === true,
      sourceAllowlist: Array.isArray(body.sourceAllowlist) ? body.sourceAllowlist.map(String).slice(0, 16) : [],
    };
    historyStatus = { persisted: true, sourceCount: historyConfig.sourceAllowlist.length };
    sendJson(response, 200, { success: true, data: { config: structuredClone(historyConfig), status: structuredClone(historyStatus) } }, method);
    return;
  }

  if (url.pathname === '/api/remote-resources/config' && method === 'POST') {
    const body = await readPayload(request);
    const next = {
      remoteChannels: Array.isArray(body.remoteChannels) ? structuredClone(body.remoteChannels) : [],
      weatherSources: Array.isArray(body.weatherSources) ? structuredClone(body.weatherSources) : [],
    };
    for (const source of next.weatherSources) {
      const previous = remoteResources.weatherSources.find((candidate) => String(candidate.id) === String(source.id));
      if (source.apiKey === '********') source.apiKey = previous?.apiKey || '';
      if (source.authHeader === '********') source.authHeader = previous?.authHeader || '';
    }
    remoteResources = next;
    sendJson(response, 200, { success: true, data: maskedRemoteResources() }, method);
    return;
  }

  if (url.pathname === '/api/remote-resources/weather-test' && method === 'POST') {
    const body = await readPayload(request);
    const id = String(body.id || url.searchParams.get('id') || '');
    const source = remoteResources.weatherSources.find((candidate) => candidate.id === id && candidate.enabled !== false);
    if (!source) {
      sendJson(response, 404, { success: false, error: 'Weather source unavailable', code: 404 }, method);
      return;
    }
    const rateLimited = weatherTestLastRequest[id] === true;
    weatherTestLastRequest[id] = true;
    sendJson(response, 200, {
      success: true,
      data: rateLimited ? {
        ...responseFor('/api/remote-resources/weather-test', new URLSearchParams({ id })).data,
        accepted: false,
        rateLimited: true,
        message: 'Weather minimum refresh interval is 5 minutes',
      } : {
        sourceId: id, label: source.label, status: 'pending', pending: true,
        updatedMs: 0, forecastUpdatedMs: 0, retryAfterMs: 300_000,
        preview: { forecast: [] }, accepted: true,
      },
    }, method);
    return;
  }

  if ((url.pathname === '/api/screen-panel/execute' || url.pathname === '/api/system/restart' || url.pathname === '/api/periph-exec/run') && method === 'POST') {
    sendJson(response, 200, { success: true, data: { status: 'queued' } }, method);
    return;
  }

  if (url.pathname === '/api/peripherals/preflight' && method === 'POST') {
    const body = await readPayload(request);
    const candidate = normalizePeripheral(body);
    sendJson(response, 200, {
      success: true,
      allowed: true,
      data: {
        operation: body.operation || 'add',
        allowed: true,
        requiresConfirmation: false,
        requiredAcknowledgements: 0,
        issues: [],
        candidate,
      },
    }, method);
    return;
  }

  if (url.pathname === '/api/periph-exec/preflight' && method === 'POST') {
    const body = await readPayload(request);
    const rule = body.rule && typeof body.rule === 'object' ? body.rule : body;
    const issues = [];
    const actions = Array.isArray(rule.actions) ? rule.actions : [];
    const actionContracts = actions.slice(0, 4).map((action, index) => {
      const target = peripherals.find(item => item.id === action.targetPeriphId);
      const contract = { index, actionType: action.actionType, coverage: 'unknown', schema: 'unknown',
        result: 'unknown', idempotence: 'unknown', replay: 'unknown', timeoutScope: 'unknown',
        timeoutBudgetMs: null, fault: 'unknown', physicalVerified: false, runtimeAdmitted: false };
      if (target?.enabled === false) issues.push({code: 'ACTION_TARGET_DISABLED', severity: 'warning',
        path: `rule.actions[${index}].targetPeriphId`, message: 'Target peripheral is currently disabled'});
      if ([6, 7, 9].includes(action.actionType)) issues.push({code: 'DANGEROUS_SYSTEM_ACTION', severity: 'warning',
        path: `rule.actions[${index}]`, message: 'Restart, reset or OTA requires explicit confirmation'});
      // The fixture describes only this known native GPIO path. Other driver
      // combinations remain unknown; no fabricated capability/physical claim.
      if ([0, 1, 13, 14, 30].includes(action.actionType) && target?.type === 12 && !target.params?.passive &&
          ![1, 'expander'].includes(target.params?.ioSource)) Object.assign(contract, {
        coverage: 'described', schema: 'gpio.level', result: 'local_acceptance_only',
        idempotence: action.actionType === 30 ? 'non_idempotent' : 'absolute_command_same_config',
        replay: 'no_automatic_replay', fault: 'no_automatic_safe_state',
      });
      return contract;
    });
    sendJson(response, 200, {
      success: true,
      allowed: true,
      data: {
        operation: body.operation || 'add', allowed: true,
        requiresConfirmation: issues.length > 0, riskLevel: issues.length ? 'medium' : 'low', issues, candidate: rule,
        actionContractsVersion: 1, actionContractsTruncated: actions.length > 4, actionContracts,
      },
    }, method);
    return;
  }

  if (url.pathname === '/api/peripherals' && method === 'POST') {
    const candidate = normalizePeripheral(await readPayload(request));
    const existing = peripherals.findIndex((item) => item.id === candidate.id);
    if (existing >= 0) peripherals[existing] = candidate;
    else peripherals.push(candidate);
    sendJson(response, 200, { success: true, message: 'Peripheral added' }, method);
    return;
  }

  if (url.pathname === '/api/peripherals/update' && method === 'POST') {
    const candidate = normalizePeripheral(await readPayload(request));
    const existing = peripherals.findIndex((item) => item.id === candidate.id);
    if (existing >= 0) peripherals[existing] = candidate;
    sendJson(response, 200, { success: existing >= 0, message: existing >= 0 ? 'Peripheral updated' : 'Peripheral not found' }, method);
    return;
  }

  if ((url.pathname === '/api/periph-exec' || url.pathname === '/api/periph-exec/update') && method === 'POST') {
    const rule = await readPayload(request);
    const existing = executionRules.findIndex((item) => item.id === rule.id);
    if (existing >= 0) executionRules[existing] = rule;
    else executionRules.push(rule);
    sendJson(response, 200, { success: true, message: existing >= 0 ? 'Rule updated' : 'Rule added' }, method);
    return;
  }

  if (url.pathname.startsWith('/api/')) {
    sendJson(response, 200, responseFor(url.pathname, url.searchParams, targetProfile), method);
    return;
  }

  if (method !== 'GET' && method !== 'HEAD') {
    response.writeHead(405, { Allow: 'GET, HEAD' });
    response.end();
    return;
  }

  const asset = resolveStaticFile(url.pathname);
  if (!asset) {
    response.writeHead(404, { 'Cache-Control': 'no-store' });
    response.end();
    return;
  }

  const headers = {
    'Cache-Control': 'no-store',
    'Content-Type': mimeType(asset.logicalPath),
    'Content-Length': statSync(asset.file).size,
  };
  if (asset.gzip) {
    headers['Content-Encoding'] = 'gzip';
    headers.Vary = 'Accept-Encoding';
  }
  response.writeHead(200, headers);
  if (method === 'HEAD') response.end();
  else createReadStream(asset.file).pipe(response);
});

server.listen(port, host, () => {
  console.log(`[offline-ui] serving gzip Web assets at http://${host}:${port}`);
});

function shutdown() {
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1000).unref();
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
