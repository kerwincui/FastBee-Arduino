# M07 任务栈单次快照与生命周期修复

2026-10-04，r20首轮预检之后的主仓后继开发。r20集中诊断r2实际执行本模块26项Host回归全部通过；r2整体仍FAIL，未形成九环境完整候选。没有设备访问、部署、硬件动作或全任务扫描实机测量。

## 已实现的入口

`POST /api/system/task-stack-snapshot`需要有效登录和开发者模式，只在明确请求时调用SDK。没有新增GET、页面轮询、后台全量采样、任务创建或配置写入。`SystemRouteHandler`只挂载独立路由；后端在独立`TaskStackSnapshot.cpp`，响应序列化在`TaskStackSnapshotJson.h`。

入口在HealthMonitor缺失、SEVERE及以上、内部8-bit空闲不足32768字节或最大连续块不足8192字节时不扫描。准入不以PSRAM空闲量抵消内部DRAM不足。单飞原子字放内部DRAM，以一次CAS取得，不等待在途采集；每次已准入的尝试至少间隔30秒，容量或分配失败也进入冷却。冷却用32位无符号差值，覆盖常规millis回绕；内存拒绝/权限拒绝不建立扫描任务。

最多48个任务，SDK数组按需在内部DRAM分配，另有48条纯标量副本，均不占HTTP任务的大数组栈；SDK scratch在返回路由前释放，标量副本在JSON构建完成后释放。数组内不保存业务指针，JSON溢出或序列化超过8192字节时整体503，不发部分成功。`makeJsonDocument(8192)`是现有PSRAM分配选择余量，不是JSON堆分配硬上限；总堆峰值与响应成本仍待集中测试和设备测量。

## 证据语义

每条只复制SDK已经填入结构的`taskNumber/priority/highWaterBytes/stateCode`，不读取`pcTaskName`、`xHandle`、`pxStackBase`，不在SDK返回后再向任务句柄查询。任务删除不会让序列化访问悬空名字或任务对象。`name=null`、`nameStatus=unavailable`；任务编号只按当前SDK/本次启动理解，不是跨重启业务身份。

有效结果为`status=observed`、`completeAtCapture=true`、`observedCount`及`tasks`。调用前数量超过48、SDK返回0、返回数量越界、分配失败、时钟异常、忙或冷却均为`status=unknown`，`observedCount/tasks=null`、`completeAtCapture=false`。未知不当空列表、不返回旧成功缓存。SDK数量变化时保留`countBefore/countAfter/countChangedAroundCapture`，只描述本次SDK返回的任务列表，不承诺响应时仍存在。

`stackUnit=bytes`符合当前ESP-IDF端口；0是有效的危险低水位，不能改成未知。HWM是该任务存续期内曾经剩余的最小栈，不是当前空闲或最大业务负载证明。`sampledAtUptimeMs`使用64位启动单调时间，不是UTC；`captureDurationUs`是SDK调用前后实测时间差，不包括JSON传输。SDK调用可能挂起调度较长时间，`schedulerPausePossible=true`、`hardDeadlineEnforced=false`、`atomicSnapshot=false`；固定条目容量和事后耗时都不是硬实时截止保证。

身份/开发模式拒绝保持原HTTP错误。后端未观察结果用HTTP200、`success=false`及上述unknown理由；响应JSON无法完整建立时503。`retryAfterMs`只说明当前冷却剩余，不授权客户端自动轮询。

## 旧定期/重启路径修复

原`xTaskGetHandle(name)`之后再查询HWM无法保全短寿命任务。HealthMonitor每60秒日志和`getTaskStackInfo`现在只通过`uxTaskGetStackHighWaterMark(nullptr)`读调用者自身，显示`current-task`范围，不假定它是loopTask，不再伪装成原7/9个任务实时列表。无后端时显式observed=false，水位内部sentinel为UINT32_MAX，不写0。

RestartDiagnostics保留RTC结构布局、魔数、原有快照和校验流程。新采集的三个旧命名字段使用UINT16_MAX未知sentinel；日志与诊断包将它们明确标为unknown/null，旧未保全生命周期的数字不再用于推断loopTask栈溢出。没有在低内存重启或周期检查中增加全任务扫描。

## 准备的验证与剩余验收

`test_task_stack_snapshot.py`直接编译真实后端，提取真实路由、HealthMonitor两个方法和RestartDiagnostics采集方法，运行真实ArduinoJson序列化；SDK、HTTP、认证、分配器和时间为替身。26项实际通过，覆盖48上限/49拒绝、SDK0/数量变化、悬空指针哨兵、零水位、重入单飞、首次0时间/回绕冷却、内部空闲/最大块/SEVERE、两种OOM、时钟异常、长调用真实耗时、权限、缺健康监控、JSON失败、TRACE关闭和无ESP未知、旧路径只读调用者。原证据保存在r2的`host-unit-all.log`，不代替实际FreeRTOS耗时或WDT验收。

全仓Host/Native旧任务名单/HWM断言已检索；现有Native重启测试镜像RTC布局仍一致，没有将旧字段改为新的实时数值。新增真实采集测试专门约束三个sentinel和Web null。Auth/Logger/Weather等自己的`nullptr`测量仍保留，未把它们替换成全量扫描。

仍需统一Host/Native/九环境构建、实际ABI scratch字节数/map成本，随后在新授权稳定候选上逐板验证任务峰值、扫描耗时、HTTP延迟、内部堆、最差业务路径和WDT。历史COM15诊断的17任务不是全部板型或最差峰值保证；48超限必须保持未知。接口开发不关闭M07、整包G03、实物矩阵或24h/7d/V14。
