// HTTP请求配置和拦截器
// axios全局配置
axios.defaults.baseURL = 'http://fastbee.local';
axios.defaults.timeout = 3000; // 3秒超时
axios.defaults.withCredentials = true;
axios.defaults.headers.common['Content-Type'] = `application/json`;

// 配置请求拦截器
axios.interceptors.request.use(
    function (config) {
        // 发送请求前可修改配置，如添加统一请求头
        console.log('请求即将发送:', config.url);
        // config.headers['X-Requested-With'] = 'XMLHttpRequest';
        
        // 添加认证令牌
        const token = localStorage.getItem('auth_token');
        if (token) config.headers.Authorization = `Bearer ${token}`;
        // config.headers.Cookie = "fastbee_session=authenticated";
        
        return config; // 必须返回配置对象
    },
    function (error) {
        // 处理请求错误（如配置无效）
        return Promise.reject(error);
    }
);

// 配置响应拦截器
axios.interceptors.response.use(
    function (response) {
        // 对2xx范围内的响应状态码进行处理
        console.log('收到响应:', response.status);
        // 可以统一处理响应数据格式，例如直接返回data部分
        return response.data;
    },
    function (error) {
        // 处理2xx范围外的响应错误
        if (error.response) {
            // 服务器有响应但状态码错误
            console.error('请求失败，状态码:', error.response.status);
             switch (error.response.status) {
                case 401:
                    // 未授权，跳转到登录页
                    window.location.href = '/login';
                    break;
                case 403:
                    Notification.warn(`权限不足`, '接口响应');
                    break;
                case 404:
                    Notification.warn(`资源不存在`, '接口响应');
                    break;
                case 500:
                    Notification.warn(`服务器内部错误`, '接口响应');
                    break;
                default:
                    Notification.warn(error.response.data?.error || '请求失败', '接口响应');
            }
        } else if (error.request) {
            // 请求已发出但无响应（网络问题）
            Notification.error(`网络错误，无服务器响应`, '提示');
        } else {
            // 请求配置出错
            Notification.error('请求配置错误:' + error.message, '提示');
        }
        return Promise.reject(error); // 将错误继续抛出，以便在具体请求的catch中处理
    }
);