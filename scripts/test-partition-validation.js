/**
 * Test: Validate partition table format and consistency
 * Purpose: Prevent future issues with incorrect filesystem types in partition tables
 */

const fs = require('fs');
const path = require('path');

// Configuration
const PARTITIONS_DIR = path.join(__dirname, '..', 'partitions');
const REQUIRED_ENVS = ['esp32c3-F4R0', 'esp32c6-F4R0', 'esp32s3-F16R8', 'esp32-F4R0'];

class PartitionTableValidator {
    constructor() {
        this.errors = [];
        this.warnings = [];
    }

    /**
     * Validate all partition tables in the partitions directory
     */
    validateAllTables() {
        console.log('\n=== Starting Partition Table Validation ===\n');
        
        const files = fs.readdirSync(PARTITIONS_DIR)
            .filter(f => f.endsWith('.csv') && !f.includes('README'));
        
        console.log(`Found ${files.length} partition tables:`);
        files.forEach(f => console.log(`  - ${f}`));
        
        let allValid = true;
        files.forEach(file => {
            const fullPath = path.join(PARTITIONS_DIR, file);
            const isValid = this.validateTable(fullPath, file);
            if (!isValid) {
                allValid = false;
            }
        });

        console.log('\n' + '='.repeat(50));
        if (this.errors.length > 0) {
            console.log(`❌ VALIDATION FAILED: ${this.errors.length} error(s)`);
            this.errors.forEach(err => console.log(`  ERROR: ${err}`));
        } else if (this.warnings.length > 0) {
            console.log(`⚠️ VALIDATION PASSED WITH WARNINGS`);
            this.warnings.forEach(warn => console.log(`  WARNING: ${warn}`));
        } else {
            console.log('✅ VALIDATION PASSED - All partition tables are valid!');
        }
        console.log('='.repeat(50) + '\n');

        return allValid && this.errors.length === 0;
    }

    /**
     * Validate a single partition table file
     */
    validateTable(filePath, fileName) {
        console.log(`\n🔍 Validating: ${fileName}`);
        console.log('-'.repeat(50));
        
        const content = fs.readFileSync(filePath, 'utf-8');
        const lines = content.split('\n');
        
        let hasLittleFS = false;
        let invalidOffsets = false;
        const partitions = [];
        
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i].trim();
            
            // Skip comments and empty lines
            if (!line || line.startsWith('#')) continue;
            
            const parts = line.split(',').map(p => p.trim());
            if (parts.length < 5) continue;
            
            const [name, type, subtype, offsetStr, sizeStr] = parts;
            
            // Arduino-ESP32's LittleFS implementation uses the historical
            // "spiffs" partition label by default. Accept that exact alias,
            // while rejecting unrelated SPIFFS partitions.
            if (type === 'data' &&
                (subtype === 'littlefs' || name.toLowerCase().includes('littlefs') ||
                 (name === 'spiffs' && subtype === 'spiffs'))) {
                hasLittleFS = true;
            }
            
            // Validate offset format (should be hex starting with 0x)
            if (!offsetStr.match(/^0x[0-9a-fA-F]+$/)) {
                this.errors.push(`[${fileName}:${i+1}] Invalid offset format: ${offsetStr} (${name})`);
                invalidOffsets = true;
            }
            
            // Validate size format
            if (!sizeStr.match(/^0x[0-9a-fA-F]+$/)) {
                this.errors.push(`[${fileName}:${i+1}] Invalid size format: ${sizeStr} (${name})`);
            } else if (!invalidOffsets) {
                partitions.push({
                    name,
                    type,
                    subtype,
                    offset: parseInt(offsetStr, 16),
                    size: parseInt(sizeStr, 16),
                    line: i + 1
                });
            }
        }

        // Check capacity and overlap using the flash size encoded in the name.
        const sizeMatch = fileName.match(/(4|8|16)MB/i);
        const flashCapacity = sizeMatch ? parseInt(sizeMatch[1], 10) * 0x100000 : null;
        if (flashCapacity) {
            partitions.forEach(partition => {
                if (partition.offset + partition.size > flashCapacity) {
                    this.errors.push(`[${fileName}:${partition.line}] Partition '${partition.name}' exceeds ${sizeMatch[1]}MB flash`);
                }
            });
            const sorted = [...partitions].sort((a, b) => a.offset - b.offset);
            for (let i = 1; i < sorted.length; i++) {
                const previous = sorted[i - 1];
                if (previous.offset + previous.size > sorted[i].offset) {
                    this.errors.push(`[${fileName}] Partitions '${previous.name}' and '${sorted[i].name}' overlap`);
                }
            }
        }

        // 4MB images intentionally use one app slot; larger images retain OTA.
        const is4MB = fileName.includes('4MB');
        const appPartitions = partitions.filter(partition => partition.type === 'app');
        if (is4MB && appPartitions.some(partition => partition.subtype === 'ota_1')) {
            this.errors.push(`[${fileName}] 4MB layout must not contain an ota_1 application slot`);
        }
        if (!is4MB && sizeMatch && appPartitions.length < 2) {
            this.errors.push(`[${fileName}] ${sizeMatch[1]}MB layout must contain two OTA application slots`);
        }

        if (is4MB && !hasLittleFS) {
            this.warnings.push(`[${fileName}] No LittleFS partition found (expected for 4MB flash)`);
        }
        
        if (invalidOffsets) {
            this.errors.push(`[${fileName}] Contains invalid offset formats`);
            return false;
        }
        
        console.log(`  ✅ Has LittleFS: ${hasLittleFS ? 'Yes' : 'No'}`);
        console.log(`  ✅ LittleFS label: ${hasLittleFS ? 'Compatible' : 'Not present'}`);
        console.log(`  ✅ Offset format: ${invalidOffsets ? 'INVALID' : 'Valid'}`);
        
        return this.errors.filter(error => error.startsWith(`[${fileName}`)).length === 0;
    }
}

/**
 * Pre-commit hook simulation
 * Run before committing any changes to partitions/*.csv files
 */
function preCommitCheck() {
    console.log('\n🛡️  Running pre-commit validation...\n');
    
    const validator = new PartitionTableValidator();
    const isValid = validator.validateAllTables();
    
    if (!isValid) {
        console.log('\n❌ COMMIT BLOCKED: Fix validation errors before committing!\n');
        process.exit(1);
    }
    
    console.log('\n✅ Pre-commit check passed! You may commit.\n');
    process.exit(0);
}

// Command-line interface
if (require.main === module) {
    const args = process.argv.slice(2);
    
    if (args.includes('--help') || args.includes('-h')) {
        console.log(`
Partition Table Validator

Usage: node scripts/test-partition-validation.js [options]

Options:
  --help, -h     Show this help message
  --check-only   Run only without blocking commits
  
Examples:
  node scripts/test-partition-validation.js       # Full validation
  node scripts/test-partition-validation.js --check-only  # Check only mode
        `);
        process.exit(0);
    }
    
    // Run validation
    preCommitCheck();
}

// Export for use in other modules
module.exports = {
    PartitionTableValidator,
    validateAllTables: () => new PartitionTableValidator().validateAllTables(),
    preCommitCheck
};
