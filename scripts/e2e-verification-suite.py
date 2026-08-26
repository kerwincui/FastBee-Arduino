#!/usr/bin/env python3
"""
FastBee Device End-to-End Verification Suite
Purpose: Complete validation of all deployed devices
Version: 1.0.0
"""

import sys
import subprocess
import time
import json
from datetime import datetime
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, field
from enum import Enum


class TestStatus(Enum):
    """Test result status"""
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    PENDING = "PENDING"
    ERROR = "ERROR"


@dataclass
class TestResult:
    """Individual test case result"""
    name: str
    category: str
    status: TestStatus
    duration_ms: int = 0
    message: str = ""
    details: dict = field(default_factory=dict)


@dataclass
class DeviceHealth:
    """Device health information"""
    device_name: str
    com_port: str
    ip_address: Optional[str] = None
    mac_address: Optional[str] = None
    firmware_version: str = "Unknown"
    last_boot: Optional[str] = None
    uptime_seconds: int = 0
    wifi_rssi: int = -999
    mqtt_connected: bool = False
    peripherals_count: int = 0
    rules_count: int = 0
    heap_free_kb: int = 0
    cpu_freq_mhz: int = 240


class E2EVersificationSuite:
    """Complete end-to-end verification for FastBee devices"""
    
    def __init__(self):
        self.results: List[TestResult] = []
        self.device_health: Dict[str, DeviceHealth] = {}
        self.start_time = datetime.now()
        
    def run_all_tests(self, target_devices: List[str] = None):
        """Execute comprehensive test suite"""
        
        print("\n" + "="*70)
        print("  FastBee Device E2E Verification Suite")
        print("="*70)
        print(f"\nStart Time: {self.start_time.isoformat()}")
        
        # Discovery Phase
        print("\n[Phase 1] Device Discovery")
        print("-"*70)
        discovered_devices = self._discover_devices(target_devices)
        
        if not discovered_devices:
            print("⚠️ No devices found. Please configure WiFi credentials.")
            return
        
        # Health Check
        print(f"\n[Phase 2] Health Checks ({len(discovered_devices)} devices)")
        print("-"*70)
        for device in discovered_devices:
            self._check_device_health(device)
        
        # Functional Tests
        print(f"\n[Phase 3] Functional Tests")
        print("-"*70)
        self._run_functional_tests(discovered_devices)
        
        # API Tests
        print(f"\n[Phase 4] API Endpoint Tests")
        print("-"*70)
        self._run_api_tests(discovered_devices)
        
        # MQTT Tests
        print(f"\n[Phase 5] MQTT Communication Tests")
        print("-"*70)
        self._run_mqtt_tests(discovered_devices)
        
        # Report Generation
        self._generate_report()
    
    def _discover_devices(self, target_devices: List[str] = None) -> List[DeviceHealth]:
        """Discover and verify device connectivity"""
        
        # Known IP mappings (should be updated after WiFi config)
        known_devices = [
            {"name": "ESP32-C3", "port": "COM4", "ip_guess": ["192.168.5.34"], "env": "esp32c3-F4R0"},
            {"name": "ESP32-C6", "port": "COM11", "ip_guess": ["192.168.5.35"], "env": "esp32c6-F4R0"},
            {"name": "ESP32-S3", "port": "COM15", "ip_guess": ["192.168.5.36"], "env": "esp32s3-F8R4"},
            {"name": "ESP32 Classic", "port": "COM16", "ip_guess": ["10.52.122.2"], "env": "esp32-F4R0"}
        ]
        
        if target_devices:
            known_devices = [d for d in known_devices if d["name"] in target_devices]
        
        for device_info in known_devices:
            device = DeviceHealth(
                device_name=device_info["name"],
                com_port=device_info["port"]
            )
            
            # Try each IP guess
            for ip in device_info["ip_guess"]:
                try:
                    response = subprocess.run(
                        ["curl", "--noproxy", "*", "--max-time", "3", "-s", 
                         f"http://{ip}/api/system/status"],
                        capture_output=True, text=True, timeout=5
                    )
                    
                    if response.returncode == 0 and len(response.stdout) > 50:
                        device.ip_address = ip
                        device.last_boot = datetime.now().isoformat()
                        break
                        
                except Exception as e:
                    continue
            
            if device.ip_address:
                self.device_health[device.device_name] = device
                print(f"  ✓ {device.device_name} @ {device.ip_address}")
            else:
                print(f"  ⚠ {device.device_name}: Not reachable at expected IPs")
        
        return list(self.device_health.values())
    
    def _check_device_health(self, device: DeviceHealth):
        """Perform comprehensive health check"""
        
        if not device.ip_address:
            return
        
        try:
            # System Status
            response = subprocess.run(
                ["curl", "--noproxy", "*", "--max-time", "5", "-s",
                 f"{device.ip_address}/api/system/status"],
                capture_output=True, text=True, timeout=10
            )
            
            if response.returncode != 0:
                self.results.append(TestResult(
                    name=f"Health:{device.device_name}:System Status",
                    category="health",
                    status=TestStatus.FAIL,
                    message="Failed to get system status"
                ))
                return
            
            # Parse JSON response
            data = json.loads(response.stdout)
            
            # Extract metrics
            if 'data' in data:
                system_data = data['data']
                device.heap_free_kb = system_data.get('heapFree', 0)
                device.mqtt_connected = system_data.get('mqttState') == 'connected'
                
                self.results.append(TestResult(
                    name=f"Health:{device.device_name}",
                    category="health",
                    status=TestStatus.PASS,
                    details={
                        "heap_kb": device.heap_free_kb,
                        "mqtt_state": system_data.get('mqttState'),
                        "mode": system_data.get('mode')
                    }
                ))
                
        except Exception as e:
            self.results.append(TestResult(
                name=f"Health:{device.device_name}",
                category="health",
                status=TestStatus.ERROR,
                message=str(e)
            ))
    
    def _run_functional_tests(self, devices: List[DeviceHealth]):
        """Run functional tests on devices"""
        
        for device in devices:
            if not device.ip_address:
                continue
            
            print(f"\n  Testing {device.device_name}...")
            
            # Web page accessibility
            start_time = time.time()
            response = subprocess.run(
                ["curl", "--noproxy", "*", "--max-time", "5", "-s",
                 f"{device.ip_address}/"],
                capture_output=True, text=True, timeout=10
            )
            
            elapsed = int((time.time() - start_time) * 1000)
            
            if response.returncode == 0 and len(response.content) > 100:
                self.results.append(TestResult(
                    name=f"WebAccess:{device.device_name}",
                    category="functional",
                    status=TestStatus.PASS,
                    duration_ms=elapsed,
                    details={"size_bytes": len(response.content)}
                ))
                print(f"    ✓ Web page ({len(response.content)} bytes)")
            else:
                self.results.append(TestResult(
                    name=f"WebAccess:{device.device_name}",
                    category="functional",
                    status=TestStatus.FAIL,
                    duration_ms=elapsed,
                    message="No valid web response"
                ))
                print(f"    ✗ Web access failed")
            
            # Config availability
            response = subprocess.run(
                ["curl", "--noproxy", "*", "--max-time", "5", "-s",
                 f"{device.ip_address}/api/device/config"],
                capture_output=True, text=True, timeout=10
            )
            
            if response.returncode == 0:
                self.results.append(TestResult(
                    name=f"ConfigAPI:{device.device_name}",
                    category="functional",
                    status=TestStatus.PASS,
                    details={"size_bytes": len(response.content)}
                ))
                print(f"    ✓ Configuration API")
            else:
                self.results.append(TestResult(
                    name=f"ConfigAPI:{device.device_name}",
                    category="functional",
                    status=TestStatus.FAIL,
                    message="Config API not accessible"
                ))
                print(f"    ✗ Config API failed")
    
    def _run_api_tests(self, devices: List[DeviceHealth]):
        """Test all API endpoints"""
        
        api_endpoints = [
            "/api/system/status",
            "/api/system/info",
            "/api/system/web-runtime",
            "/api/network/status",
            "/api/mqtt/status",
            "/api/device/config",
            "/api/peripherals",
            "/api/periph-exec/rules"
        ]
        
        for endpoint in api_endpoints:
            print(f"\n  Testing API: {endpoint}")
            
            for device in devices:
                if not device.ip_address:
                    continue
                
                try:
                    response = subprocess.run(
                        ["curl", "--noproxy", "*", "--max-time", "5", "-s",
                         f"{device.ip_address}{endpoint}"],
                        capture_output=True, text=True, timeout=10
                    )
                    
                    status = TestStatus.PASS if response.returncode == 0 else TestStatus.FAIL
                    
                    self.results.append(TestResult(
                        name=f"API:{endpoint}:{device.device_name}",
                        category="api",
                        status=status,
                        details={
                            "status_code": response.returncode,
                            "size_bytes": len(response.content),
                            "response_sample": response.content[:100] if response.content else ""
                        }
                    ))
                    
                except Exception as e:
                    self.results.append(TestResult(
                        name=f"API:{endpoint}:{device.device_name}",
                        category="api",
                        status=TestStatus.ERROR,
                        message=str(e)
                    ))
    
    def _run_mqtt_tests(self, devices: List[DeviceHealth]):
        """Test MQTT communication"""
        
        for device in devices:
            if not device.ip_address:
                continue
            
            print(f"\n  MQTT Tests for {device.device_name}")
            
            # Check MQTT status via API
            try:
                response = subprocess.run(
                    ["curl", "--noproxy", "*", "--max-time", "5", "-s",
                     f"{device.ip_address}/api/mqtt/status"],
                    capture_output=True, text=True, timeout=10
                )
                
                if response.returncode == 0:
                    data = json.loads(response.stdout)
                    mqtt_connected = data.get('data', {}).get('connected', False)
                    
                    self.results.append(TestResult(
                        name=f"MQTTStatus:{device.device_name}",
                        category="mqtt",
                        status=TestStatus.PASS if mqtt_connected else TestStatus.SKIP,
                        details={"connected": mqtt_connected}
                    ))
                    
                    print(f"    MQTT: {'Connected ✓' if mqtt_connected else 'Not connected (may need config)'}")
                else:
                    self.results.append(TestResult(
                        name=f"MQTTStatus:{device.device_name}",
                        category="mqtt",
                        status=TestStatus.FAIL,
                        message="MQTT status API unavailable"
                    ))
                    
            except Exception as e:
                self.results.append(TestResult(
                    name=f"MQTTStatus:{device.device_name}",
                    category="mqtt",
                    status=TestStatus.ERROR,
                    message=str(e)
                ))
    
    def _generate_report(self):
        """Generate comprehensive test report"""
        
        total_tests = len(self.results)
        passed = sum(1 for r in self.results if r.status == TestStatus.PASS)
        failed = sum(1 for r in self.results if r.status == TestStatus.FAIL)
        skipped = sum(1 for r in self.results if r.status == TestStatus.SKIP)
        errors = sum(1 for r in self.results if r.status == TestStatus.ERROR)
        
        print("\n" + "="*70)
        print("  TEST SUMMARY REPORT")
        print("="*70)
        
        print(f"\nTotal Tests: {total_tests}")
        print(f"✓ Passed:    {passed}")
        print(f"✗ Failed:    {failed}")
        print(f"⏸ Skipped:   {skipped}")
        print(f"❌ Errors:    {errors}")
        
        if total_tests > 0:
            success_rate = (passed / total_tests) * 100
            print(f"\nSuccess Rate: {success_rate:.1f}%")
        
        # Detailed breakdown by category
        categories = set(r.category for r in self.results)
        
        for category in sorted(categories):
            cat_tests = [r for r in self.results if r.category == category]
            cat_passed = sum(1 for r in cat_tests if r.status == TestStatus.PASS)
            
            print(f"\n{category.upper()}:")
            print(f"  Total: {len(cat_tests)}, Passed: {cat_passed}")
            
            # Show failures
            failures = [r for r in cat_tests if r.status in [TestStatus.FAIL, TestStatus.ERROR]]
            if failures:
                for f in failures[:5]:  # Show first 5 failures
                    print(f"    ✗ {f.name}: {f.message or f.status.value}")
        
        # Save detailed report
        report = {
            "timestamp": self.start_time.isoformat(),
            "duration_seconds": (datetime.now() - self.start_time).total_seconds(),
            "total_tests": total_tests,
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "errors": errors,
            "success_rate_percent": (passed / total_tests * 100) if total_tests > 0 else 0,
            "results": [{
                "name": r.name,
                "category": r.category,
                "status": r.status.value,
                "duration_ms": r.duration_ms,
                "message": r.message,
                "details": r.details
            } for r in self.results],
            "device_health": {k: vars(v) for k, v in self.device_health.items()}
        }
        
        report_path = ".pio/puzhong/e2e-verification-report.json"
        with open(report_path, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        
        print(f"\nDetailed report saved to: {report_path}")
        print("\n" + "="*70)


def main():
    """Main entry point"""
    
    print("\n" + "#"*70)
    print("#  FastBee Device Verification Suite")
    print("#  Version: 1.0.0")
    print("#"*70)
    
    suite = E2EVersificationSuite()
    suite.run_all_tests()


if __name__ == "__main__":
    main()
