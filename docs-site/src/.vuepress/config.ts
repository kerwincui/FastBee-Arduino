import { defineUserConfig } from 'vuepress'
import { defaultTheme } from '@vuepress/theme-default'
import { sidebarZh } from './sidebar/zh'
import { sidebarEn } from './sidebar/en'
import { navbarZh } from './navbar/zh'
import { navbarEn } from './navbar/en'

export default defineUserConfig({
  title: 'FastBee-Arduino 设备端文档',
  description: 'FastBee-Arduino 零代码物联网固件文档',
  base: '/',

  locales: {
    '/': {
      lang: 'zh-CN',
      title: 'FastBee-Arduino 设备端文档',
      description: 'FastBee-Arduino 零代码物联网固件文档',
    },
    '/en/': {
      lang: 'en-US',
      title: 'FastBee-Arduino Device Docs',
      description: 'FastBee-Arduino zero-code IoT firmware documentation',
    },
  },

  theme: defaultTheme({
    logo: '/images/favicon.gif',
    locales: {
      '/': {
        sidebar: sidebarZh,
        navbar: navbarZh,
        selectLanguageText: '语言',
        selectLanguageName: '简体中文',
      },
      '/en/': {
        sidebar: sidebarEn,
        navbar: navbarEn,
        selectLanguageText: 'Language',
        selectLanguageName: 'English',
      },
    },
  }),
})
