/** The legacy motor status endpoint uses POST even though it only reads FC03. */
export function isReadOnlyMotorStatus(method, url, body, contentType = '') {
  let target;
  try { target = new URL(url); } catch { return false; }
  if (method !== 'POST' || target.pathname !== '/api/modbus/motor/control' || target.search) return false;
  const type = contentType.split(';')[0].trim().toLowerCase();
  let values;
  try {
    if (type === 'application/x-www-form-urlencoded') {
      const params = new URLSearchParams(body);
      if ([...params.keys()].length !== 2 || params.getAll('action').length !== 1 || params.getAll('slaveAddress').length !== 1) return false;
      values = Object.fromEntries(params);
    } else if (type === 'application/json') {
      values = JSON.parse(body);
    } else return false;
  } catch { return false; }
  if (!values || Array.isArray(values) || Object.keys(values).sort().join(',') !== 'action,slaveAddress') return false;
  const address = String(values.slaveAddress);
  return values.action === 'readStatus' && /^\d{1,3}$/.test(address) && Number(address) >= 1 && Number(address) <= 247;
}

/** Validate the request governor's POST wrapper for GET-only batch reads. */
export function isReadOnlyBatch(method, url, body, contentType = '') {
  let target;
  try { target = new URL(url); } catch { return false; }
  if (method !== 'POST' || target.pathname !== '/api/batch' || target.search) return false;
  if (contentType.split(';')[0].trim().toLowerCase() !== 'application/json') return false;
  let payload;
  try { payload = JSON.parse(body); } catch { return false; }
  if (!payload || Array.isArray(payload) || Object.keys(payload).join(',') !== 'requests' ||
      !Array.isArray(payload.requests) || payload.requests.length < 1 || payload.requests.length > 5) return false;
  return payload.requests.every(item => {
    if (!item || Array.isArray(item) || typeof item !== 'object') return false;
    const keys = Object.keys(item);
    if (keys.some(key => !['url', 'params', 'method'].includes(key)) ||
        typeof item.url !== 'string' || !item.url.startsWith('/api/') ||
        (item.method !== undefined && item.method !== 'GET')) return false;
    return item.params === undefined ||
      (!!item.params && !Array.isArray(item.params) && typeof item.params === 'object');
  });
}
