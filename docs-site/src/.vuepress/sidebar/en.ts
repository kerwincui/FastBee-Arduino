import type { SidebarConfig } from 'vuepress'

export const sidebarEn: SidebarConfig = {
  '/en/device/': [
    {
      text: 'Getting Started',
      collapsible: true,
      children: [
        { text: 'Overview', link: '/en/device/README.md' },
        { text: 'Introduction', link: '/en/device/overview.md' },
        { text: 'Quick Start', link: '/en/device/quick-start.md' },
        { text: 'Edition Comparison', link: '/en/device/edition-comparison.md' },
        { text: 'User Manual', link: '/en/device/user-manual.md' },
        { text: 'Hardware Selection', link: '/en/device/hardware-equipment.md' },
      ],
    },
    {
      text: 'Deployment & Verification',
      collapsible: true,
      children: [
        { text: 'Online Flasher', link: '/en/device/esp32-flasher.md' },
        { text: 'Flashing & Deployment', link: '/en/device/flashing-testing.md' },
        { text: 'Testing', link: '/en/device/testing.md' },
        { text: 'Release Checklist', link: '/en/device/stability-release-checklist.md' },
      ],
    },
    {
      text: 'System Features',
      collapsible: true,
      children: [
        { text: 'System Overview', link: '/en/device/system/README.md' },
      ],
    },
    {
      text: 'Protocols',
      collapsible: true,
      children: [
        { text: 'Protocol Overview', link: '/en/device/protocols/README.md' },
      ],
    },
    {
      text: 'Peripherals & Rules',
      collapsible: true,
      children: [
        { text: 'Peripheral Configuration', link: '/en/device/peripherals/README.md' },
        { text: 'Sensor Guide', link: '/en/device/peripherals/sensor-guide-complete.md' },
        { text: 'Peripheral Execution', link: '/en/device/periph-exec/README.md' },
      ],
    },
    {
      text: 'Examples',
      collapsible: true,
      children: [
        { text: 'Examples Overview', link: '/en/device/examples/README.md' },
      ],
    },
    {
      text: 'Development & Release',
      collapsible: true,
      children: [
        { text: 'Architecture', link: '/en/device/architecture.md' },
        { text: 'Core Framework', link: '/en/device/core-framework.md' },
        { text: 'Project Structure', link: '/en/device/project-structure.md' },
        { text: 'Development Guide', link: '/en/device/development-guide.md' },
        { text: 'Code Change Guidelines', link: '/en/device/code-change-guidelines.md' },
        { text: 'Build Configuration', link: '/en/device/build-config.md' },
        { text: 'Resource Tuning', link: '/en/device/resource-tuning.md' },
        { text: 'TCP Connection Budget', link: '/en/device/tcp-connection-budget.md' },
        { text: 'Commercial License', link: '/en/device/commercial-license.md' },
      ],
    },
  ],
}
