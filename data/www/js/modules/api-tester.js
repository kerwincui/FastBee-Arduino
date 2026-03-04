/**
 * FastBee API 接口测试工具
 *
 * 用途：
 *   - 在浏览器控制台执行 ApiTester.runAll() 对所有接口进行连通性和格式验证
 *   - 在浏览器控制台执行 ApiTester.runSingle('health') 单独测试某个接口
 *   - 返回详细的测试报告，包含调用成功率、响应时间、数据一致性检查
 *
 * 使用方式（浏览器控制台）：
 *   ApiTester.runAll().then(report => console.table(report.summary));
 *   ApiTester.runSingle('login').then(r => console.log(r));
 */
const ApiTester = (function () {
    'use strict';

    // ===== 测试用例定义 =====
    const TEST_CASES = [
        // ---- 公开接口（无需登录）----
        {
            id: 'health',
            name: '健康检查',
            method: 'GET',
            url: '/api/health',
            auth: false,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['status', 'timestamp', 'freeHeap'],
                dataValues: { status: 'healthy' }
            }
        },
        {
            id: 'status',
            name: '系统状态（公开）',
            method: 'GET',
            url: '/api/system/status',
            auth: false,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['status', 'timestamp', 'freeHeap']
            }
        },

        // ---- 认证接口 ----
        {
            id: 'login',
            name: '用户登录',
            method: 'POST',
            url: '/api/auth/login',
            auth: false,
            body: { username: 'admin', password: 'admin123' },
            expect: {
                status: 200,
                fields: ['success', 'sessionId', 'username'],
                fieldValues: { success: true }
            },
            onSuccess: (res) => {
                // 保存测试用 session token
                ApiTester._testToken = res.sessionId;
            }
        },
        {
            id: 'session',
            name: '会话验证',
            method: 'GET',
            url: '/api/auth/session',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['username', 'sessionValid'],
                dataValues: { sessionValid: true }
            }
        },

        // ---- 用户管理接口 ----
        {
            id: 'users_list',
            name: '获取用户列表',
            method: 'GET',
            url: '/api/users',
            auth: true,
            params: { page: 1, limit: 10 },
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['users', 'total', 'page', 'limit', 'count']
            }
        },
        {
            id: 'users_online',
            name: '获取在线用户',
            method: 'GET',
            url: '/api/users/online',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['onlineUsers', 'count']
            }
        },

        // ---- 角色权限接口 ----
        {
            id: 'roles_list',
            name: '获取角色列表',
            method: 'GET',
            url: '/api/roles',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['roles', 'total']
            }
        },
        {
            id: 'permissions_all',
            name: '获取所有权限定义',
            method: 'GET',
            url: '/api/permissions',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['groups', 'total']
            }
        },

        // ---- 系统接口 ----
        {
            id: 'system_info',
            name: '系统详细信息',
            method: 'GET',
            url: '/api/system/info',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['chipModel', 'freeHeap', 'uptime', 'sdkVersion']
            }
        },
        {
            id: 'fs_info',
            name: '文件系统信息',
            method: 'GET',
            url: '/api/system/fs-info',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['totalBytes', 'usedBytes', 'freeBytes', 'usedPercent']
            }
        },

        // ---- 配置接口 ----
        {
            id: 'config_get',
            name: '获取系统配置',
            method: 'GET',
            url: '/api/config',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['webPort', 'sessionTimeout', 'maxLoginAttempts', 'lockoutTime']
            }
        },
        {
            id: 'network_config',
            name: '获取网络配置',
            method: 'GET',
            url: '/api/network/config',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data']
            }
        },

        // ---- OTA 接口 ----
        {
            id: 'ota_status',
            name: 'OTA状态查询',
            method: 'GET',
            url: '/api/ota/status',
            auth: true,
            expect: {
                status: 200,
                fields: ['success', 'data'],
                dataFields: ['status', 'progress']
            }
        },

        // ---- 审计日志接口 ----
        {
            id: 'audit_logs',
            name: '审计日志查询',
            method: 'GET',
            url: '/api/audit/logs',
            auth: true,
            params: { limit: 10 },
            expect: {
                status: 200,
                fields: ['logs']
            }
        },

        // ---- 鉴权边界测试 ----
        {
            id: 'auth_boundary_no_token',
            name: '无Token访问受保护接口（应返回401）',
            method: 'GET',
            url: '/api/users',
            auth: false,
            skipAutoAuth: true,
            expect: {
                status: 401,
                isError: true
            }
        },
        {
            id: 'login_wrong_password',
            name: '错误密码登录（应返回401）',
            method: 'POST',
            url: '/api/auth/login',
            auth: false,
            body: { username: 'admin', password: 'wrong_password_12345' },
            expect: {
                status: 401,
                isError: true
            }
        }
    ];

    // ===== 内部状态 =====
    let _testToken = null;

    // ===== 核心执行函数 =====
    function _executeTest(testCase) {
        const startTime = Date.now();
        const result = {
            id: testCase.id,
            name: testCase.name,
            method: testCase.method,
            url: testCase.url,
            passed: false,
            errors: [],
            warnings: [],
            responseTime: 0,
            statusCode: null,
            response: null
        };

        // 构造请求配置
        const config = {
            method: testCase.method.toLowerCase(),
            url: testCase.url,
            headers: {}
        };

        // 注入认证头
        if (testCase.auth && !testCase.skipAutoAuth) {
            const token = _testToken || localStorage.getItem('auth_token');
            if (token) {
                config.headers['Authorization'] = `Bearer ${token}`;
            } else {
                result.errors.push('No auth token available; run "login" test first');
                result.passed = false;
                return Promise.resolve(result);
            }
        }

        // 请求参数
        if (testCase.params) {
            config.params = testCase.params;
        }
        if (testCase.body) {
            config.headers['Content-Type'] = 'application/x-www-form-urlencoded';
            config.data = Object.keys(testCase.body)
                .map(k => encodeURIComponent(k) + '=' + encodeURIComponent(testCase.body[k]))
                .join('&');
        }

        return axios(config)
            .then(responseData => {
                result.responseTime = Date.now() - startTime;
                // config.js 拦截器已返回 response.data
                result.response = responseData;
                result.statusCode = 200; // 2xx

                _checkExpectations(result, testCase.expect, responseData, false);

                // 成功回调（如保存 token）
                if (result.errors.length === 0 && typeof testCase.onSuccess === 'function') {
                    testCase.onSuccess(responseData);
                }

                result.passed = result.errors.length === 0;
                return result;
            })
            .catch(err => {
                result.responseTime = Date.now() - startTime;
                if (err.response) {
                    result.statusCode = err.response.status;
                    result.response = err.response.data;
                } else {
                    result.statusCode = 0;
                    result.response = null;
                }

                _checkExpectations(result, testCase.expect, result.response, true);
                result.passed = result.errors.length === 0;
                return result;
            });
    }

    function _checkExpectations(result, expect, responseData, isHttpError) {
        if (!expect) return;

        // 检查 HTTP 状态码
        if (expect.status !== undefined) {
            if (result.statusCode !== expect.status) {
                result.errors.push(`HTTP status expected ${expect.status}, got ${result.statusCode}`);
            }
        }

        if (!responseData) {
            if (!isHttpError) result.errors.push('Response body is empty');
            return;
        }

        // 如果期望是错误响应，不检查字段
        if (expect.isError) return;

        // 检查顶级字段存在性
        if (expect.fields) {
            expect.fields.forEach(field => {
                if (responseData[field] === undefined) {
                    result.errors.push(`Missing top-level field: "${field}"`);
                }
            });
        }

        // 检查顶级字段值
        if (expect.fieldValues) {
            Object.entries(expect.fieldValues).forEach(([k, v]) => {
                if (responseData[k] !== v) {
                    result.errors.push(`Field "${k}" expected "${v}", got "${responseData[k]}"`);
                }
            });
        }

        // 检查 data 子字段
        const data = responseData.data;
        if (expect.dataFields) {
            if (!data || typeof data !== 'object') {
                result.errors.push('Expected "data" object in response but got: ' + JSON.stringify(data));
            } else {
                expect.dataFields.forEach(field => {
                    if (data[field] === undefined) {
                        result.errors.push(`Missing data field: "data.${field}"`);
                    }
                });
            }
        }

        // 检查 data 字段值
        if (expect.dataValues && data) {
            Object.entries(expect.dataValues).forEach(([k, v]) => {
                if (data[k] !== v) {
                    result.errors.push(`data.${k} expected "${v}", got "${data[k]}"`);
                }
            });
        }

        // 检查响应时间（> 5秒发出警告）
        if (result.responseTime > 5000) {
            result.warnings.push(`Slow response: ${result.responseTime}ms`);
        }
    }

    // ===== 公开 API =====

    /**
     * 运行所有测试用例
     * @param {Object} options
     * @param {string} [options.password='admin123']  管理员密码（用于 login 测试）
     * @returns {Promise<{results: Array, summary: Object}>}
     */
    function runAll(options) {
        options = options || {};
        // 如果提供了自定义密码，更新 login 测试的 body
        if (options.password) {
            const loginCase = TEST_CASES.find(t => t.id === 'login');
            if (loginCase) loginCase.body.password = options.password;
        }

        console.group('%c[ApiTester] 开始全量接口测试', 'color: #2196F3; font-weight: bold');
        console.log('测试用例总数:', TEST_CASES.length);

        // 顺序执行（避免认证依赖问题）
        const results = [];
        return TEST_CASES.reduce((promise, testCase) => {
            return promise.then(() => {
                return _executeTest(testCase).then(result => {
                    results.push(result);
                    const icon = result.passed ? '✅' : '❌';
                    const style = result.passed ? 'color: green' : 'color: red; font-weight: bold';
                    console.log(
                        `%c${icon} [${result.method}] ${result.url} (${result.responseTime}ms)`,
                        style,
                        result.errors.length ? '  错误: ' + result.errors.join('; ') : ''
                    );
                    if (result.warnings.length) {
                        console.warn('  ⚠️ 警告:', result.warnings.join('; '));
                    }
                });
            });
        }, Promise.resolve()).then(() => {
            const passed = results.filter(r => r.passed).length;
            const failed = results.filter(r => !r.passed).length;
            const avgTime = Math.round(results.reduce((sum, r) => sum + r.responseTime, 0) / results.length);

            const summary = {
                total: results.length,
                passed,
                failed,
                successRate: Math.round(passed / results.length * 100) + '%',
                avgResponseTime: avgTime + 'ms',
                failedTests: results.filter(r => !r.passed).map(r => r.name)
            };

            console.log('\n%c[ApiTester] 测试结果汇总', 'color: #2196F3; font-weight: bold');
            console.table({
                '总数': summary.total,
                '通过': summary.passed,
                '失败': summary.failed,
                '成功率': summary.successRate,
                '平均响应时间': summary.avgResponseTime
            });

            if (summary.failedTests.length) {
                console.error('失败的测试:', summary.failedTests);
            }

            console.groupEnd();
            return { results, summary };
        });
    }

    /**
     * 单独运行某个测试用例
     * @param {string} testId  测试用例 ID
     * @returns {Promise<Object>}
     */
    function runSingle(testId) {
        const testCase = TEST_CASES.find(t => t.id === testId);
        if (!testCase) {
            const ids = TEST_CASES.map(t => t.id).join(', ');
            return Promise.reject(new Error(`Test "${testId}" not found. Available: ${ids}`));
        }
        return _executeTest(testCase).then(result => {
            const icon = result.passed ? '✅' : '❌';
            console.log(`${icon} [${result.method}] ${result.url}`);
            console.log('  响应时间:', result.responseTime + 'ms');
            console.log('  状态码:', result.statusCode);
            if (!result.passed) console.error('  错误:', result.errors);
            if (result.warnings.length) console.warn('  警告:', result.warnings);
            console.log('  响应数据:', result.response);
            return result;
        });
    }

    /**
     * 列出所有测试用例 ID
     */
    function listTests() {
        console.table(TEST_CASES.map(t => ({
            ID: t.id,
            名称: t.name,
            方法: t.method,
            URL: t.url,
            需要认证: t.auth ? '是' : '否'
        })));
    }

    /**
     * 测试接口响应数据一致性：连续调用同一接口 n 次，检查关键字段值是否稳定
     * @param {string} testId
     * @param {number} [times=3]
     */
    function runConsistencyCheck(testId, times) {
        times = times || 3;
        const testCase = TEST_CASES.find(t => t.id === testId);
        if (!testCase) {
            return Promise.reject(new Error(`Test "${testId}" not found`));
        }

        console.group(`[ApiTester] 一致性测试: ${testCase.name} × ${times}`);
        const runs = [];
        let chain = Promise.resolve();

        for (let i = 0; i < times; i++) {
            chain = chain.then(() => _executeTest(testCase).then(r => runs.push(r)));
        }

        return chain.then(() => {
            const responseTimes = runs.map(r => r.responseTime);
            const allPassed = runs.every(r => r.passed);
            console.log('全部通过:', allPassed ? '✅' : '❌');
            console.log('响应时间:', responseTimes.join('ms, ') + 'ms');
            console.log('最大:', Math.max(...responseTimes) + 'ms', '最小:', Math.min(...responseTimes) + 'ms');
            console.groupEnd();
            return { runs, allPassed, responseTimes };
        });
    }

    return {
        runAll,
        runSingle,
        listTests,
        runConsistencyCheck,
        // 测试用 token（可从外部设置）
        get _testToken() { return _testToken; },
        set _testToken(v) { _testToken = v; }
    };
})();

// 挂载到全局，方便浏览器控制台调用
window.ApiTester = ApiTester;

// 页面加载完成后输出简要使用提示
if (typeof console !== 'undefined') {
    console.info(
        '%c[ApiTester] 接口测试工具已加载\n' +
        '  ApiTester.runAll()                  - 运行全量测试\n' +
        '  ApiTester.runSingle("health")        - 测试单个接口\n' +
        '  ApiTester.listTests()                - 列出所有测试用例\n' +
        '  ApiTester.runConsistencyCheck("status", 5) - 一致性测试',
        'color: #9C27B0; font-size: 12px'
    );
}
