// 补丁脚本：将本轮多设备验证沉淀的经验写入 FastBee-doc/src/device 文档
// 用法：node patches\doc-update-device-20260820.js
const fs = require('fs');
const path = require('path');

const DOC_ROOT = 'D:/project/gitlab/FastBee-doc/src/device';

function patch(relPath, anchor, insertion, mode = 'after') {
  const fp = path.join(DOC_ROOT, relPath);
  let s = fs.readFileSync(fp, 'utf8');
  const isCRLF = s.includes('\r\n');
  const norm = (t) => (isCRLF ? t.replace(/\r?\n/g, '\r\n') : t);
  if (s.includes(insertion.slice(0, 60)) || s.includes(norm(insertion.slice(0, 60)))) {
    console.log(`[skip] ${relPath} already patched`);
    return;
  }
  let a = anchor;
  let idx = s.indexOf(a);
  if (idx < 0 && isCRLF) {
    a = norm(anchor);
    idx = s.indexOf(a);
  }
  if (idx < 0) {
    console.log(`[FAIL] anchor not found in ${relPath}: ${anchor.slice(0, 60)}...`);
    process.exitCode = 1;
    return;
  }
  const ins = norm(insertion);
  if (mode === 'after') {
    const end = idx + a.length;
    s = s.slice(0, end) + ins + s.slice(end);
  } else {
    s = s.slice(0, idx) + ins + s.slice(idx);
  }
  fs.writeFileSync(fp, s, 'utf8');
  console.log(`[ok] ${relPath} patched (${mode} anchor, ${isCRLF ? 'CRLF' : 'LF'})`);
}

// ── 1. testing.md：矩阵 allowedStatuses 机制 + 断言编写注意事项 ──
patch(
  'deploy/testing.md',
  '新增或调整 API 时，先更新 `device-api-test-matrix.json`，再执行 smoke/soak，确保裁剪行为和状态码都符合预期。',
  '\n\n矩阵中普通检查项（无 `type`）支持 `allowedStatuses` 数组：命中其中任一 HTTP 状态码即判通过（跳过语义检查），用于表达"固件设计内的确定性非 200 响应"。典型案例：`rule-scripts` 允许 503——ruleScript 特性默认 `userEnabled=false`，未启用时 `RuleScriptManager` 不初始化（mutex 为 NULL），`/api/rule-script` 恒定返回 `503 "Service busy, try again"`，这是特性开关语义而非故障。新增此类用例时同步确认 `/api/features/status` 中对应特性的 `compiled/userEnabled/active` 三态。\n\n断言编写另需注意 PowerShell 5.1 的两个坑：① `ConvertFrom-Json` 会把空数组 `[]` 转成 `$null`，`peripherals`、`periph-exec-rules` 等列表型端点在设备无配置时 `data` 为空数组，不能以 `data -eq $null` 判失败；② capabilities 断言不得检查固件已移除的字段（full/unified 档无 `tcp`/`http`/`coap`），无 PSRAM 机型也不能强制要求 `psramTotal>0`。'
);

// ── 2. testing.md：FAQ 新增两条（WebGuard 死区 + 日志 rotate 超时） ──
patch(
  'deploy/testing.md',
  '恢复 STA 后 mDNS 重新绑定、MQTT 自动重连。',
  '\n\n### 无 PSRAM 设备高负载后所有 API 持续 503（WebGuard 熔断锁死死区）\n\n现象：classic ESP32（无 PSRAM）经高负载 soak/浏览器压测后，所有 `/api/*` 持续返回 `503 "Server low memory, request rejected"`（带 `Retry-After: 5`），停止负载 1 小时以上仍不恢复；静态页面正常（熔断仅拦 `/api/`），ping 可达。根因（2026-08-20 实测）：DRAM 落在两个阈值之间的**死区**——高于 HealthMonitor CRITICAL 自愈重启阈值（无 PSRAM 机型 8KB，需持续 90s 才触发安全重启），又低于 WebMemoryGuard 恢复阈值（`free8≥24KB` 且 `blk8≥9KB`，见 `MemoryBudget.h`），既不自愈重启也不解除熔断；且熔断拦截全部 `/api/`（含 `/api/system/restart`），无任何非 `/api` 的远程重启路由。**处理：物理断电重启**；恢复后对无 PSRAM 机型跑 soak 应降低负载（如 `-DelayMs 1500`），full 矩阵 500ms 间隔对 DRAM 余量不足的机型过重。\n\n### soak 零星超时集中在文件系统类端点（日志 rotate 同步写）\n\n现象：soak 长稳中偶发个别失败（≤0.5%），全部为超时且集中在 `/api/filesystem`、`/api/files`、`/api/logs*`，集中在一两轮内、随后完全自愈，重启计数与 heap 趋势均正常。根因：LoggerSystem 日志 flush/rotate 的 LittleFS 同步写瞬时阻塞 Web 任务。判定：失败端点分布与重启/泄漏指标即可与真故障区分；后续可将 rotate 异步化彻底消除。'
);

// ── 2.1 testing.md FAQ：补充串口 RTS 复位脉冲恢复手段（2026-08-20 实证成功） ──
patch(
  'deploy/testing.md',
  '**处理：物理断电重启**',
  '；断电前可先尝试**串口 RTS 复位脉冲**（pyserial 打开串口发 0.15s RTS 脉冲：`s.dtr=False; s.rts=True; sleep(0.15); s.rts=False`），2026-08-20 实测 classic ESP32 熔断锁死且可 ping 但 Web 挂起时该手段成功恢复，无需物理断电。另注意一种中间态：设备可 ping、`/api/health` 超时——STA 在线但 Web/lwIP 挂起，同样优先 RTS 脉冲复位。'
);

// ── 3. troubleshooting.md：4.2 之后新增 4.3 熔断锁死 ──
patch(
  'deploy/troubleshooting.md',
  '### 4.2 接口间歇性返回 503 "temporarily unavailable (memory pressure)"\n\n设备处于 SEVERE 内存压力等级，重量级接口被门控熔断。减少并发请求/等待 `Retry-After` 后重试；长期出现请检查是否误烧 full Web 镜像或存在内存泄漏（观察 `[STATUS]` 心跳的 heap 趋势）。',
  '\n\n### 4.3 所有 API 持续 503 且长时间不恢复（熔断锁死死区，无 PSRAM 机型）\n\n- **现象**：高负载压测后 `/api/*` 全部返回 `503 "Server low memory"`，停止负载超过 1 小时仍不恢复；静态页面可访问、ping 正常。\n- **原因**：DRAM 卡在 WebMemoryGuard 恢复阈值（free8≥24KB）与 HealthMonitor CRITICAL 自愈重启阈值（无 PSRAM 机型 8KB）之间的死区，既不能自动解除熔断，也不触发 90s 安全重启；`/api/system/restart` 同样被熔断拦截，无远程恢复路径。\n- **处理**：**物理断电重启**；断电前先试串口 RTS 复位脉冲（pyserial 发 0.15s RTS 脉冲，见测试指南 FAQ），2026-08-20 实测该手段可恢复，无需物理断电。预防：无 PSRAM 机型（classic ESP32 F4R0）避免 full 矩阵高频率（≤500ms 间隔）长时间压测，soak 建议 `-DelayMs 1500` 起步。\n- 详见 [测试指南 FAQ：WebGuard 熔断锁死死区](./testing.md)。'
);

// ── 3.1 troubleshooting.md 4.3：已打补丁的文档补充 RTS 恢复手段 ──
patch(
  'deploy/troubleshooting.md',
  '- **处理**：**物理断电重启**。',
  '\n\n  > 断电前可先尝试串口 RTS 复位脉冲（pyserial 打开串口发 0.15s RTS 脉冲：`s.dtr=False; s.rts=True; sleep(0.15); s.rts=False`），2026-08-20 实测 classic ESP32 锁死后该手段成功恢复，无需物理断电。'
);

// ── 4. api-reference.md：rule-script 503 语义 + logs 参数说明 ──
patch(
  'dev/api-reference.md',
  '| GET/POST | `/api/rule-script` | RuleScript 规则 CRUD |',
  '\n\n> ruleScript 特性默认未启用（`userEnabled=false`）。未启用时 `RuleScriptManager` 不初始化，`/api/rule-script` 系列恒定返回 `503 "Service busy, try again"`，属设计行为；可通过 `/api/features/status` 查询 `compiled/userEnabled/active` 三态确认。'
);

patch(
  'dev/api-reference.md',
  '| GET | `/api/logs` | 日志查询（支持 `list`、`info`、`clear` 子路径） |',
  '\n\n> `/api/logs` 的所有查询参数（`limit`、`file`、`tail` 等）均为可选，缺省请求必须正常返回（历史版本曾因未判空直接解引用 `getParam("limit")` 导致 LoadProhibited 崩溃，2026-08-20 修复，`LogRouteHandler` 增加 `hasParam` 判空）。'
);

console.log('doc patch complete');
