/**
 * ESP32 Web 静态资源 Gzip 压缩脚本
 * 
 * 功能：
 * 1. 删除 data/www 目录下所有旧的 .gz 文件
 * 2. 压缩所有 HTML/JS/CSS 文件生成新的 .gz 文件
 * 
 * 使用方法：
 *   node scripts/gzip-www.js
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const WWW_DIR = path.join(__dirname, '..', 'data', 'www');

// 需要压缩的文件扩展名
const COMPRESS_EXTENSIONS = ['.html', '.js', '.css'];

// 统计信息
let stats = {
    deleted: 0,
    compressed: 0,
    totalOriginalSize: 0,
    totalCompressedSize: 0
};

/**
 * 递归遍历目录
 */
function walkDir(dir, callback) {
    if (!fs.existsSync(dir)) {
        console.error(`目录不存在: ${dir}`);
        return;
    }
    
    const files = fs.readdirSync(dir);
    files.forEach(file => {
        const filePath = path.join(dir, file);
        const stat = fs.statSync(filePath);
        
        if (stat.isDirectory()) {
            walkDir(filePath, callback);
        } else {
            callback(filePath);
        }
    });
}

/**
 * 删除所有旧的 .gz 文件
 */
function deleteOldGzFiles() {
    console.log('\n[Step 1] 删除旧的 .gz 文件...');
    
    walkDir(WWW_DIR, (filePath) => {
        if (filePath.endsWith('.gz')) {
            fs.unlinkSync(filePath);
            stats.deleted++;
            console.log(`  删除: ${path.relative(WWW_DIR, filePath)}`);
        }
    });
    
    console.log(`  共删除 ${stats.deleted} 个 .gz 文件`);
}

/**
 * 压缩文件
 */
function compressFile(filePath) {
    const ext = path.extname(filePath).toLowerCase();
    
    if (!COMPRESS_EXTENSIONS.includes(ext)) {
        return;
    }
    
    const content = fs.readFileSync(filePath);
    const compressed = zlib.gzipSync(content, { level: 9 });
    const gzPath = filePath + '.gz';
    
    fs.writeFileSync(gzPath, compressed);
    
    const originalSize = content.length;
    const compressedSize = compressed.length;
    const ratio = ((1 - compressedSize / originalSize) * 100).toFixed(1);
    
    stats.compressed++;
    stats.totalOriginalSize += originalSize;
    stats.totalCompressedSize += compressedSize;
    
    console.log(`  压缩: ${path.relative(WWW_DIR, filePath)} (${originalSize} -> ${compressedSize}, -${ratio}%)`);
}

/**
 * 压缩所有文件
 */
function compressAllFiles() {
    console.log('\n[Step 2] 压缩 HTML/JS/CSS 文件...');
    
    walkDir(WWW_DIR, compressFile);
    
    const totalRatio = ((1 - stats.totalCompressedSize / stats.totalOriginalSize) * 100).toFixed(1);
    console.log(`\n  共压缩 ${stats.compressed} 个文件`);
    console.log(`  总大小: ${stats.totalOriginalSize} -> ${stats.totalCompressedSize} bytes (-${totalRatio}%)`);
}

/**
 * 主函数
 */
function main() {
    console.log('========================================');
    console.log('  ESP32 Web 静态资源 Gzip 压缩工具');
    console.log('========================================');
    console.log(`目标目录: ${WWW_DIR}`);
    
    deleteOldGzFiles();
    compressAllFiles();
    
    console.log('\n========================================');
    console.log('  压缩完成！');
    console.log('========================================\n');
}

main();

