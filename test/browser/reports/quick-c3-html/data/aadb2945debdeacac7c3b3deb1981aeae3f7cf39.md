# Instructions

- Following Playwright test failed.
- Explain why, be concise, respect Playwright best practices.
- Provide a snippet of code with the fix, if possible.

# Test info

- Name: 18-linkage.spec.ts >> Suite-18: 多模块综合联动测试 >> LNK-010: Modbus多设备轮询→多主题发布 @quick
- Location: suites\18-linkage.spec.ts:303:7

# Error details

```
Test timeout of 90000ms exceeded while setting up "authPage".
```

# Page snapshot

```yaml
- generic [ref=e3]:
  - combobox "语言/Language" [ref=e5] [cursor=pointer]:
    - option "中文" [selected]
    - option "English"
  - generic [ref=e6]:
    - img "FastBee Logo" [ref=e7]
    - heading "FastBee Arduino" [level=1] [ref=e8]
  - group [ref=e11]:
    - generic [ref=e12]: 用户名
    - textbox "用户名" [ref=e13]:
      - /placeholder: 请输入用户名
      - text: admin
    - generic [ref=e14]: 密码
    - textbox "密码" [ref=e15]:
      - /placeholder: 请输入密码
      - text: admin123
    - generic [ref=e16] [cursor=pointer]:
      - checkbox "保持登录 WP-13 安全加固：不再保存明文密码" [ref=e17]
      - generic [ref=e18]: 保持登录
      - generic [ref=e19]: WP-13 安全加固：不再保存明文密码
    - button "登录系统" [ref=e20] [cursor=pointer]
```