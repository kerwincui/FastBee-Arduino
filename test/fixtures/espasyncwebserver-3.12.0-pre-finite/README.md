# ESPAsyncWebServer 3.12.0 固定旧补丁测试输入

这是已经含 FastBee 旧补丁、尚未加入有限响应进展 hook 的 3.12.0 安装快照，用于验证首次升级、幂等、四文件整体预检、写失败回滚和旧新混合拒绝。它不是未经修改的 upstream 发行版，也不是完整可编译库。运行摘取测试另外明确选择同一环境的 Web 3.12.0 / AsyncTCP 3.5.0，不使用此不完整快照代替完整依赖。

四个源文件及 metadata 来源：2026-10-06 冻结的 `artifacts/zero-progress-bound-preparation-20261006-r1/baseline/.pio/libdeps/esp32c3-F4R0/ESPAsyncWebServer/`。经 `artifacts/zero-progress-old312-fixture-20261006-r1/fixture/ESPAsyncWebServer/` 原字节复制，未修改换行或 vendor 内容。

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| library.json | 1445 | 1654ae4c24b82d8833323647e760a7faca61600ecd6533afd980821bd6e01168 |
| src/ESPAsyncWebServer.h | 62854 | cce1722206e166a2693f66d5f087968df6b51c2308a1aeb61710e8969e9db81b |
| src/WebResponseImpl.h | 9039 | 6e47bda4b501cb8e9efd93cf2aea4c575f1467e093e92182a3d35abc8bdb291c |
| src/WebResponses.cpp | 44632 | 7e2f152ebdcefd617b5c78f80ef6213dab141deb91cf61cb4bc72de6bf398542 |
| src/WebRequest.cpp | 55083 | 9ac3bb39c0942a0c3d1b2674ab8e9d4af39678275a789a7d49ee5f28501be1a2 |
| LICENSE | 7651 | a5681bf9b05db14d86776930017c647ad9e6e56ff6bbcfdf21e5848288dfaf1b |

`LICENSE` 独立取自同机当前 `.pio/libdeps/esp32c3-F4R0/ESPAsyncWebServer/LICENSE`，按其原字节复制，保留 GNU LGPL v3 文本；它没有被追记到旧的五文件 artifact 中。四个源文件的 `SPDX-License-Identifier: LGPL-3.0-or-later` 和原作者版权声明、metadata 的 `license: LGPL-3.0` 均原样保留。上游来源以 metadata 为准：`https://github.com/ESP32Async/ESPAsyncWebServer.git`。不重新许可，不将本 fixture 描述成完整上游源码或完整第三方再分发审查。

`zero_progress_support.verify_old_fixture()` 对五项测试输入和 LICENSE 作 SHA256 绑定，并核对版本和不存在新 hook；运行时只复制 metadata 与四源文件到独立临时目录，原 fixture 保持不变。若需要更新基线，应另行审阅来源、摘要、许可和旧到新行为，不能把已升级安装目录覆盖本 fixture 以取得测试通过。
