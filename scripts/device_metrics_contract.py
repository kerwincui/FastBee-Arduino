"""Validate the historical /api/system/metrics raw JSON contract only."""
import math

def uint(value, field, maximum=0xffffffff):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('RAW_METRICS_REQUIRED_UINT:' + field)
    return value

def raw_metrics(body):
    if not isinstance(body,dict) or any(k in body for k in ('success','data','error','code','message','reason')):
        raise ValueError('RAW_METRICS_CONTRACT_REQUIRED')
    fixed = {'heap','mqtt','sse','poll','uptime','boot_time_ms','memguard','stability','isr'}
    if not fixed.issubset(body) or any(k not in fixed and not k.startswith('pools.') for k in body):
        raise ValueError('RAW_METRICS_FIELDS_REQUIRED')
    def finite(value):
        if isinstance(value,dict):
            for item in value.values(): finite(item)
        elif isinstance(value,list):
            for item in value: finite(item)
        elif isinstance(value,float) and not math.isfinite(value):
            raise ValueError('RAW_METRICS_NONFINITE')
    finite(body)
    def group(name):
        obj = body[name]
        if not isinstance(obj,dict): raise ValueError('RAW_METRICS_GROUP_REQUIRED:' + name)
        return obj
    for name,fields in [('heap',('free','min','largest_block','dram_free','dram_largest','psram_total','psram_free')),
                        ('mqtt',('queue_depth',)),('sse',('client_count',)),('poll',('duration_ms',)),
                        ('isr',('queue_overruns','queue_waiting')),
                        ('stability',('dram_history_count','uptime_hours'))]:
        obj = group(name)
        for field in fields: uint(obj.get(field), name+'.'+field)
    uint(group('heap').get('fragmentation'),'heap.fragmentation',100)
    if type(body['heap'].get('tls_supported')) is not bool: raise ValueError('RAW_METRICS_TLS_REQUIRED')
    for name in ('uptime','boot_time_ms'): uint(body[name],name)
    guard = group('memguard')
    level = uint(guard.get('level'),'memguard.level',3)
    if guard.get('level_name') != ('NORMAL','WARN','SEVERE','CRITICAL')[level]:
        raise ValueError('RAW_METRICS_GUARD_LEVEL_DISAGREES')
    for field in ('fragmentation_high','mqtts_downgraded','mqtts_paused','mqtt_stopped','mqtt_disabled',
                  'mqtt_restore_pending','modbus_stopped','periph_exec_paused','periph_exec_worker_suspended',
                  'periph_exec_worker_suspension_pending','optional_peripherals_suspended'):
        if type(guard.get(field)) is not bool: raise ValueError('RAW_METRICS_BOOL_REQUIRED:' + field)
    if guard.get('mqtts_protection') != 'pause_without_plaintext_fallback':
        raise ValueError('RAW_METRICS_MQTTS_POLICY_REQUIRED')
    for field in ('optional_peripheral_count','critical_duration_s'): uint(guard.get(field),'memguard.'+field)
    if type(body['stability'].get('leak_suspect')) is not bool: raise ValueError('RAW_METRICS_LEAK_REQUIRED')
    pools = [name for name in body if name.startswith('pools.')]
    if not pools: raise ValueError('RAW_METRICS_POOLS_REQUIRED')
    for name in pools:
        obj = group(name)
        used,cap = uint(obj.get('used'),name+'.used'),uint(obj.get('capacity'),name+'.capacity')
        uint(obj.get('exhaust'),name+'.exhaust')
        if not cap or used > cap: raise ValueError('RAW_METRICS_POOL_CAPACITY')
    return body

def unwrap_api(path, body):
    if path == '/api/system/metrics' and isinstance(body,dict) and 'success' not in body:
        return raw_metrics(body)
    if not isinstance(body,dict) or body.get('success') is not True:
        raise ValueError('API failed: ' + path)
    return body.get('data') or {}


def metrics_response(response):
    """Only a complete HTTP 200 historical metrics body can enter parsing."""
    if response.status_code != 200:
        raise ValueError('RAW_METRICS_HTTP_FAILURE:' + str(response.status_code))
    body = response.content
    declared = response.headers.get('Content-Length')
    if declared is not None:
        if not isinstance(declared, str) or not declared.isdecimal() or len(body) != int(declared):
            raise ValueError('RAW_METRICS_BODY_INCOMPLETE')
    return raw_metrics(response.json())
