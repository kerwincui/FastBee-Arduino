"""Behavior coverage for the actual historical metrics parser and HTTP contract."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('device_metrics_contract', ROOT / 'scripts/device_metrics_contract.py')
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)

# Sanitized historical route shape; tests do not make device requests.
FIXTURE = {
    'heap': {'free':26920,'min':15320,'largest_block':12788,'fragmentation':53,'dram_free':26920,
             'dram_largest':12788,'psram_total':8192,'psram_free':8101,'tls_supported':True},
    'mqtt': {'queue_depth':0}, 'sse':{'client_count':0}, 'poll':{'duration_ms':58},
    'uptime':781, 'boot_time_ms':2666,
    'memguard': {'level':0,'level_name':'NORMAL','fragmentation_high':False,'mqtts_downgraded':False,
        'mqtts_paused':False,'mqtts_protection':'pause_without_plaintext_fallback','mqtt_stopped':False,
        'mqtt_disabled':False,'mqtt_restore_pending':False,'modbus_stopped':False,'periph_exec_paused':False,
        'periph_exec_worker_suspended':False,'periph_exec_worker_suspension_pending':False,
        'optional_peripherals_suspended':False,'optional_peripheral_count':0,'critical_duration_s':0},
    'stability':{'dram_history_count':0,'leak_suspect':False,'uptime_hours':0},
    'isr':{'queue_overruns':0,'queue_waiting':0}, 'pools.async_ctx':{'used':0,'capacity':4,'exhaust':0}}

class Response:
    def __init__(self,body=None,status=200,declared=None):
        self.body = copy.deepcopy(FIXTURE if body is None else body)
        self.status_code = status
        self.content = json.dumps(self.body).encode('utf-8')
        self.headers = {'Content-Length':str(len(self.content) if declared is None else declared)}
    def json(self): return copy.deepcopy(self.body)

class DeviceMetricsContractTests(unittest.TestCase):
    def test_real_historical_raw_shape_and_complete_http_body(self):
        self.assertEqual(metrics.raw_metrics(FIXTURE),FIXTURE)
        self.assertEqual(metrics.metrics_response(Response()),FIXTURE)

    def test_missing_groups_and_scalar_fields_cannot_fabricate_metrics(self):
        for group in FIXTURE:
            with self.subTest(group=group):
                body=copy.deepcopy(FIXTURE);del body[group]
                with self.assertRaises(ValueError):metrics.raw_metrics(body)
        for group in ('heap','mqtt','sse','poll','isr','stability','memguard','pools.async_ctx'):
            for field in FIXTURE[group]:
                with self.subTest(group=group,field=field):
                    body=copy.deepcopy(FIXTURE);del body[group][field]
                    with self.assertRaises(ValueError):metrics.raw_metrics(body)

    def test_nonfinite_negative_boolean_or_string_counters_are_rejected(self):
        for value in (float('nan'),float('inf'),float('-inf'),True,False,-1,'12788',None,0xffffffff+1):
            for group,field in (('heap','dram_largest'),('isr','queue_waiting'),('pools.async_ctx','exhaust')):
                with self.subTest(value=value,field=field):
                    body=copy.deepcopy(FIXTURE);body[group][field]=value
                    with self.assertRaises(ValueError):metrics.raw_metrics(body)

    def test_error_and_explicit_failure_envelopes_never_use_raw_fallback(self):
        for field in ('success','data','error','code','message','reason'):
            body=copy.deepcopy(FIXTURE);body[field]=False if field=='success' else 'failure'
            with self.subTest(field=field),self.assertRaises(ValueError):metrics.raw_metrics(body)
        for value in (False,None,0,'true'):
            with self.subTest(success=value),self.assertRaises(ValueError):
                metrics.unwrap_api('/api/system/metrics',{'success':value,'data':FIXTURE})

    def test_guard_and_protection_metadata_remain_explicit(self):
        for level,name in enumerate(('NORMAL','WARN','SEVERE','CRITICAL')):
            body=copy.deepcopy(FIXTURE);body['memguard'].update(level=level,level_name=name)
            self.assertEqual(metrics.raw_metrics(body)['memguard']['level'],level)
            body['memguard']['level_name']='incorrect'
            with self.assertRaises(ValueError):metrics.raw_metrics(body)
        for field in ('periph_exec_paused','modbus_stopped','periph_exec_worker_suspended'):
            body=copy.deepcopy(FIXTURE);body['memguard'][field]=True
            self.assertIs(metrics.raw_metrics(body)['memguard'][field],True)
            body['memguard'][field]='false'
            with self.assertRaises(ValueError):metrics.raw_metrics(body)

    def test_pool_capacity_and_fragmentation_limits_are_checked(self):
        for field,value in (('used',5),('capacity',0),('capacity',False),('used',-1)):
            body=copy.deepcopy(FIXTURE);body['pools.async_ctx'][field]=value
            with self.subTest(field=field,value=value),self.assertRaises(ValueError):metrics.raw_metrics(body)
        body=copy.deepcopy(FIXTURE);body['heap']['fragmentation']=101
        with self.assertRaises(ValueError):metrics.raw_metrics(body)

    def test_complete_zero_readings_and_uint32_uptime_are_valid(self):
        body=copy.deepcopy(FIXTURE);body['heap']['dram_free']=0;body['heap']['dram_largest']=0
        body['uptime']=0xffffffff
        self.assertEqual(metrics.raw_metrics(body)['heap']['dram_free'],0)
        # Resource acceptance still belongs to release thresholds, not parsing.
        self.assertEqual(metrics.raw_metrics(body)['uptime'],0xffffffff)

    def test_truncated_invalid_length_or_http_failure_is_rejected(self):
        response=Response();response.headers['Content-Length']=str(len(response.content)+1)
        with self.assertRaises(ValueError):metrics.metrics_response(response)
        for status in (204,301,401,403,429,500,503):
            with self.subTest(status=status),self.assertRaises(ValueError):metrics.metrics_response(Response(status=status))
        for declared in ('nan','-1','one'):
            response=Response();response.headers['Content-Length']=declared
            with self.subTest(declared=declared),self.assertRaises(ValueError):metrics.metrics_response(response)

    def test_chunked_complete_body_can_be_parsed_without_length_header(self):
        response=Response();response.headers={}
        self.assertEqual(metrics.metrics_response(response),FIXTURE)

    def test_other_api_routes_keep_strict_success_and_existing_data_semantics(self):
        for route in ('/api/system/info','/api/modbus/status','/api/system/metrics?probe=1'):
            with self.subTest(route=route),self.assertRaises(ValueError):metrics.unwrap_api(route,FIXTURE)
            self.assertEqual(metrics.unwrap_api(route,{'success':True,'data':{'valid':0}}),{'valid':0})
            with self.assertRaises(ValueError):metrics.unwrap_api(route,{'success':False,'data':FIXTURE})
        self.assertEqual(metrics.unwrap_api('/api/system/info',{'success':True,'data':[]}),{})

    def test_unexpected_raw_shapes_and_nested_nonfinite_values_fail(self):
        for body in ([],None,{'heap':{}},{'success':False}):
            with self.subTest(body=body),self.assertRaises(ValueError):metrics.raw_metrics(body)
        body=copy.deepcopy(FIXTURE);body['unexpected']='ignored?'
        with self.assertRaises(ValueError):metrics.raw_metrics(body)
        body=copy.deepcopy(FIXTURE);body['heap']['extra']={'nested':[float('nan')]}
        with self.assertRaises(ValueError):metrics.raw_metrics(body)

if __name__ == '__main__': unittest.main()
