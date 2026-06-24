import type { SidebarConfig } from 'vuepress'

export const sidebarZh: SidebarConfig = {
  '/device/': [
    {
      text: '设备端入门',
      collapsible: true,
      children: [
        { text: '目录索引', link: '/device/README.md' },
        { text: '项目概述', link: '/device/overview.md' },
        { text: '快速入门', link: '/device/quick-start.md' },
        { text: '版本选择', link: '/device/edition-comparison.md' },
        { text: '用户手册', link: '/device/user-manual.md' },
        { text: '硬件选型', link: '/device/hardware-equipment.md' },
      ],
    },
    {
      text: '部署与验证',
      collapsible: true,
      children: [
        { text: '在线烧录工具', link: '/device/esp32-flasher.md' },
        { text: '烧录与部署', link: '/device/flashing-testing.md' },
        { text: '测试验证', link: '/device/testing.md' },
        { text: '发布清单', link: '/device/stability-release-checklist.md' },
      ],
    },
    {
      text: '系统功能',
      collapsible: true,
      children: [
        { text: '系统功能概览', link: '/device/system/README.md' },
      ],
    },
    {
      text: '通信协议',
      collapsible: true,
      children: [
        { text: '协议概览', link: '/device/protocols/README.md' },
      ],
    },
    {
      text: '外设与规则',
      collapsible: true,
      children: [
        { text: '外设配置', link: '/device/peripherals/README.md' },
        { text: '传感器完整指南', link: '/device/peripherals/sensor-guide-complete.md' },
        { text: '外设执行', link: '/device/periph-exec/README.md' },
      ],
    },
    {
      text: '示例教程',
      collapsible: true,
      children: [
        { text: '示例概览', link: '/device/examples/README.md' },
      ],
    },
    {
      text: '开发与发布',
      collapsible: true,
      children: [
        { text: '架构设计', link: '/device/architecture.md' },
        { text: '核心框架', link: '/device/core-framework.md' },
        { text: '项目结构', link: '/device/project-structure.md' },
        { text: '开发指南', link: '/device/development-guide.md' },
        { text: '代码修改规范', link: '/device/code-change-guidelines.md' },
        { text: '构建配置', link: '/device/build-config.md' },
        { text: '资源占用与功能裁剪', link: '/device/resource-tuning.md' },
        { text: 'TCP连接预算', link: '/device/tcp-connection-budget.md' },
        { text: '商用授权', link: '/device/commercial-license.md' },
      ],
    },
  ],
}
