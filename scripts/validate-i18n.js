/**
 * i18n 键对称性验证脚本
 * 用法: node scripts/validate-i18n.js
 */
const fs = require('fs');
const path = require('path');

// 读取语言文件并提取翻译对象
function extractKeys(filePath) {
    const content = fs.readFileSync(filePath, 'utf8');
    // 解析 JS 对象中的所有键（支持嵌套）
    const keys = new Set();
    // 使用正则匹配所有 key: value 行
    const keyRegex = /^\s*['"]([^'"]+)['"]\s*:/gm;
    let match;
    while ((match = keyRegex.exec(content)) !== null) {
        keys.add(match[1]);
    }
    return keys;
}

const zhFile = path.join(__dirname, '../web-src/i18n/i18n-zh-CN.js');
const enFile = path.join(__dirname, '../web-src/i18n/i18n-en.js');

const zhKeys = extractKeys(zhFile);
const enKeys = extractKeys(enFile);

// 找出不对称的键
const missingInEn = [...zhKeys].filter(k => !enKeys.has(k));
const missingInZh = [...enKeys].filter(k => !zhKeys.has(k));

console.log(`\n=== i18n 键对称性验证 ===`);
console.log(`中文键: ${zhKeys.size}个`);
console.log(`英文键: ${enKeys.size}个`);

if (missingInEn.length > 0) {
    console.log(`\n❌ 英文缺失 ${missingInEn.length} 个键:`);
    missingInEn.forEach(k => console.log(`  - ${k}`));
}

if (missingInZh.length > 0) {
    console.log(`\n❌ 中文缺失 ${missingInZh.length} 个键:`);
    missingInZh.forEach(k => console.log(`  - ${k}`));
}

if (missingInEn.length === 0 && missingInZh.length === 0) {
    console.log('\n✅ 所有键完全对称！');
}

process.exit(missingInEn.length + missingInZh.length > 0 ? 1 : 0);
