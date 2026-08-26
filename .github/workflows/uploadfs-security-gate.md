# FastBee UploadFS Security Gate - CI/CD Integration

**Version**: 1.0  
**Date**: 2026-08-25  
**Status**: Critical Security Rule  

---

## ⚠️ **Critical Security Notice**

**uploadfs is FORBIDDEN in FastBee-Arduino deployment pipeline.**

After the tragic incident on 2026-08-25 where uploadfs caused permanent device brick (Hard Fault during flash write), we have established this security gate to prevent any future occurrence of this catastrophic failure.

**Impact if violated**: Device permanent dead brick requiring GPIO0-GND physical recovery or hardware replacement.

---

## 🔒 **Security Gate Implementation**

### **Option A: GitHub Actions CI Gate**

Create file: `.github/workflows/uploadfs-blocker.yml`

```yaml
name: UploadFS Security Gate
on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main, develop]

jobs:
  check-uploadfs-forbidden:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout code
        uses: actions/checkout@v4
      
      - name: Check for uploadfs usage in scripts
        id: check-uploadfs
        run: |
          # Search for uploadfs in all files
          UPLOADFS_USAGE=$(grep -r "uploadfs" . --include="*.sh" --include="*.ps1" --include="*.py" --include="*.yml" --include="*.ini" 2>/dev/null || true)
          
          if [ -n "$UPLOADFS_USAGE" ]; then
            echo "❌ ERROR: uploadfs usage detected!"
            echo "uploadfs is FORBIDDEN and will cause device permanent brick!"
            echo ""
            echo "Detected usage:"
            echo "$UPLOADFS_USAGE"
            echo ""
            echo "Please use 'flash_dual' target instead:"
            echo '  pio run -e esp32-F4R0 --target flash_dual --upload-port COM16'
            exit 1
          else
            echo "✅ Safe deployment method detected"
            echo "::set-output name=status::passed"
          fi
      
      - name: Check platformio.ini for uploadfs targets
        run: |
          if grep -q "uploadfs" platformio.ini; then
            echo "❌ ERROR: uploadfs target found in platformio.ini!"
            exit 1
          fi
          
      - name: Block merge if violation detected
        if: steps.check-uploadfs.outcome == 'failure'
        run: |
          echo "🚫 SECURITY GATE TRIGGERED: uploadfs forbidden pattern detected"
          echo "This PR cannot be merged due to uploadfs usage."
          echo ""
          echo "Resolution steps:"
          echo "1. Remove all uploadfs references from scripts"
          echo "2. Replace with flash_dual target"
          echo "3. Consult documentation at docs/security/deployment-sop.md"
          exit 1
```

---

### **Option B: Pre-commit Hook (Recommended)**

Create file: `.git/hooks/pre-commit-uploadfs-check.sh`

```bash
#!/bin/bash
# Pre-commit hook to block uploadfs usage

echo "=== UploadFS Security Check ==="

# Find all shell scripts and PowerShell scripts
FILES=$(find . -type f \( -name "*.sh" -o -name "*.ps1" -o -name "*.bat" -o -name "*.cmd" \) ! -path "./.git/*")

FOUND_UPLOADFS=false

for FILE in $FILES; do
    if grep -q "uploadfs" "$FILE"; then
        echo "❌ uploadfs usage detected in: $FILE"
        FOUND_UPLOADFS=true
        cat "$FILE" | grep "uploadfs"
    fi
done

# Check Python scripts
PYTHON_FILES=$(find . -type f -name "*.py" ! -path "./.git/*")
for FILE in $PYTHON_FILES; do
    if grep -q "uploadfs" "$FILE"; then
        echo "❌ uploadfs usage detected in: $FILE"
        FOUND_UPLOADFS=true
    fi
done

if [ "$FOUND_UPLOADFS" = true ]; then
    echo ""
    echo "🚫 COMMIT BLOCKED: uploadfs is FORBIDDEN!"
    echo ""
    echo "Why?"
    echo "- uploadfs causes HARD FAULT during flash_write operation"
    echo "- This leads to PERMANENT DEVICE BRICK"
    echo "- Recovery requires GPIO0-GND physical intervention"
    echo "- May require hardware replacement if recovery fails"
    echo ""
    echo "Solution:"
    echo "1. Use 'flash_dual' target instead:"
    echo "   pio run -e esp32-F4R0 --target flash_dual --upload-port COM16"
    echo ""
    echo "2. For esptool.py direct flash writing:"
    echo "   esptool.py --port COM16 write_flash 0x0 factory-with-fs.bin"
    echo ""
    echo "3. Review documentation at docs/security/"
    exit 1
fi

echo "✅ No uploadfs usage detected - commit allowed"
exit 0
```

Make executable:
```bash
chmod +x .git/hooks/pre-commit-uploadfs-check.sh
```

---

### **Option C: PlatformIO Configuration Lock**

Update `platformio.ini` to add safety comments:

```ini
; ============================================================================
; DEPLOYMENT SECURITY NOTICE
; ============================================================================
; uploadfs TARGET IS FORBIDDEN AND WILL CAUSE DEVICE DESTRUCTION!
; 
; After 2026-08-25 incident analysis:
; - uploadfs causes Hard Fault during async flash write
; - LittleFS corruption leads to permanent device brick
; - Recovery requires GPIO0-GND physical intervention
; - Some devices may be unrecoverable after uploadfs failure
;
; CORRECT METHOD: Use flash_dual target
; pio run -e esp32-F4R0 --target flash_dual --upload-port COM16
; ============================================================================

[env:esp32-F4R0]
platform = espressif32
board = esp32dev
framework = arduino

; DO NOT USE THIS TARGET - IT'S FORBIDDEN BY SECURITY GATES!
; upload_target = uploadfs  ❌ FORBIDDEN ❌

; ✅ CORRECT METHOD: Use flash_dual
build_flags = 
    -D FASTBEE_LITE_PROFILE
    -D WEB_MEMORY_GUARD_ENABLED

monitor_speed = 115200
upload_speed = 921600
; ============================================================================
```

---

## 📚 **Documentation Links**

### **Required Reading Before Deployment**

1. **Device Recovery SOP** (Critical!)
   - Location: `docs/security/device-recovery-sop.md`
   - Content: GPIO0-GND shorting procedure, esptool.py commands
   - Why: If uploadfs somehow slips through (DOESN'T ALLOWED), know how to recover

2. **Uploadfs Failure RCA Report**
   - Location: `.archive-docs/TASK-18-DEVICE-RECOVERY-DIAGNOSIS.md`
   - Content: Complete analysis of the 2026-08-25 incident
   - Lessons Learned: 10 critical experience points documented

3. **Safe Flash Update Procedures**
   - Location: `docs/deployment/safe-flash-methods.md`
   - Content: flash_dual vs esptool.py direct write comparison
   - Best Practices: Windows encoding issues, port competition prevention

---

## 🚨 **Violation Handling Procedure**

If a developer somehow manages to introduce uploadfs despite security gates:

### **Immediate Actions**
1. **STOP THE BUILD** - Do not allow deployment
2. **NOTIFY TEAM LEAD** - Alert to potential risk
3. **ROLLBACK** - Revert to previous safe commit

### **Post-Incident Review**
1. **Root Cause Analysis** - How did it pass the gates?
2. **Process Improvement** - Strengthen security measures
3. **Training Update** - Add to team awareness sessions

---

## ✅ **Compliance Checklist**

Before merging any PR that includes deployment scripts:

- [ ] No `uploadfs` references in scripts
- [ ] All deployment commands use `flash_dual` target
- [ ] esptool.py commands use proper full Flash writes (not partial)
- [ ] Documentation updated if new deployment methods introduced
- [ ] Team member trained on uploadfs risks

---

## 🔔 **Alert Contacts**

If you encounter any uploadfs-related questions or violations:

- **Team Lead**: FastBee Core Team
- **Emergency Contact**: See project wiki
- **Security Incident**: Create issue labeled `security/deployment`

---

**Last Updated**: 2026-08-25  
**Security Level**: CRITICAL  
**Enforcement Status**: Active
