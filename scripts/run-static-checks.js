'use strict';

/**
 * C1: test:static 静态检查统一入口。
 *
 * 用法：
 *   node scripts/run-static-checks.js          按清单顺序执行全部静态检查
 *   node scripts/run-static-checks.js --list   只打印清单，不执行
 *
 * 任一检查失败即打印 FAILED 信息（检查名、命令、退出码），
 * 并以该检查的退出码结束；全部通过时打印汇总并以 0 退出。
 * 检查清单维护在 scripts/static-checks.manifest.js。
 */
const { spawnSync } = require('child_process');
const path = require('path');

const STATIC_CHECKS = require('./static-checks.manifest.js');
const ROOT_DIR = path.resolve(__dirname, '..');

function listChecks() {
    STATIC_CHECKS.forEach((check, index) => {
        console.log(`${index + 1}/${STATIC_CHECKS.length} ${check.name} :: ${check.command}`);
    });
}

function runChecks() {
    for (let index = 0; index < STATIC_CHECKS.length; index++) {
        const check = STATIC_CHECKS[index];
        console.log(`[static] ${index + 1}/${STATIC_CHECKS.length} ${check.name}`);
        const result = spawnSync(check.command, {
            shell: true,
            stdio: 'inherit',
            windowsHide: true,
            cwd: ROOT_DIR
        });
        if (result.error) {
            console.error(`[static] FAILED check: ${check.name}`);
            console.error(`[static] command: ${check.command}`);
            console.error(`[static] exit code: 1`);
            console.error(`[static] spawn error: ${result.error.message}`);
            return 1;
        }
        if (result.status !== 0) {
            const exitCode = result.status === null ? 1 : result.status;
            console.error(`[static] FAILED check: ${check.name}`);
            console.error(`[static] command: ${check.command}`);
            console.error(`[static] exit code: ${exitCode}`);
            return exitCode;
        }
    }
    console.log(`[static] all ${STATIC_CHECKS.length} static checks passed`);
    return 0;
}

if (process.argv.includes('--list')) {
    listChecks();
} else {
    process.exitCode = runChecks();
}
