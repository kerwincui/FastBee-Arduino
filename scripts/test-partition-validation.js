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
        let spiffsFound = false;
        let invalidOffsets = false;
        
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i].trim();
            
            // Skip comments and empty lines
            if (!line || line.startsWith('#')) continue;
            
            const parts = line.split(',').map(p => p.trim());
            if (parts.length < 5) continue;
            
            const [name, type, subtype, offsetStr, sizeStr] = parts;
            
            // Check for deprecated filesystem types
            if (subtype === 'spiffs') {
                spiffsFound = true;
                this.errors.push(`[${fileName}:${i+1}] Deprecated filesystem type 'spiffs' found (${name})`);
            }
            
            // Check for LittleFS support
            if (type === 'data' && (subtype === 'littlefs' || name.toLowerCase().includes('littlefs'))) {
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
            }
        }
        
        // Check requirements based on Flash size hint from filename
        const is4MB = fileName.includes('4MB') || fileName.includes('fastbee.csv');
        if (is4MB && !hasLittleFS) {
            this.warnings.push(`[${fileName}] No LittleFS partition found (expected for 4MB flash)`);
        }
        
        if (spiffsFound) {
            this.errors.push(`[${fileName}] Contains deprecated SPiFFS filesystem (must use LittleFS)`);
            return false;
        }
        
        if (invalidOffsets) {
            this.errors.push(`[${fileName}] Contains invalid offset formats`);
            return false;
        }
        
        console.log(`  ✅ Has LittleFS: ${hasLittleFS ? 'Yes' : 'No'}`);
        console.log(`  ✅ Deprecated SPiFFS: ${spiffsFound ? 'YES (ERROR!)' : 'No'}`);
        console.log(`  ✅ Offset format: ${invalidOffsets ? 'INVALID' : 'Valid'}`);
        
        return true;
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
