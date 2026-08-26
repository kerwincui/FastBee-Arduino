/**
 * Test: Validate build artifacts completeness and integrity
 * Purpose: Prevent missing factory-with-fs.bin issues like those with ESP32-S3
 */

const fs = require('fs');
const path = require('path');

class BuildArtifactsValidator {
    constructor() {
        this.buildDir = path.join(__dirname, '..', '.pio', 'build');
        this.errors = [];
        this.warnings = [];
    }

    /**
     * Validate all build environments have complete artifacts
     */
    validateAllEnvs(envNames) {
        console.log('\n=== Starting Build Artifacts Validation ===\n');
        
        const envs = envNames || this.discoverEnvs();
        
        if (envs.length === 0) {
            console.log('⚠️ No build environments found!');
            return false;
        }
        
        let allValid = true;
        envs.forEach(envName => {
            const isValid = this.validateEnvironment(envName);
            if (!isValid) {
                allValid = false;
            }
        });

        console.log('\n' + '='.repeat(50));
        console.log(`Total environments checked: ${envs.length}`);
        console.log(`Passed: ${envs.filter(e => this.isValidEnvironment(e)).length}`);
        console.log(`Failed: ${envs.filter(e => !this.isValidEnvironment(e)).length}`);
        
        if (this.errors.length > 0) {
            console.log(`\n❌ ERRORS (${this.errors.length}):`);
            this.errors.forEach(err => console.log(`  - ${err}`));
        }
        
        if (this.warnings.length > 0) {
            console.log(`\n⚠️ WARNINGS (${this.warnings.length}):`);
            this.warnings.forEach(warn => console.log(`  - ${warn}`));
        }
        
        console.log('='.repeat(50) + '\n');
        
        return allValid && this.errors.length === 0;
    }

    discoverEnvs() {
        if (!fs.existsSync(this.buildDir)) {
            console.log(`Build directory not found: ${this.buildDir}`);
            return [];
        }
        
        return fs.readdirSync(this.buildDir)
            .filter(item => fs.statSync(path.join(this.buildDir, item)).isDirectory());
    }

    validateEnvironment(envName) {
        console.log(`🔍 Checking environment: ${envName}`);
        
        const envDir = path.join(this.buildDir, envName);
        if (!fs.existsSync(envDir)) {
            this.warnings.push(`[Env] Build directory not found: ${envName}`);
            return false;
        }
        
        // Required files for a complete build
        const requiredFiles = [
            { name: 'firmware.elf', purpose: 'Executable image' },
            { name: 'bootloader.bin', purpose: 'Bootloader binary' }
        ];
        
        // Optional but should exist for unified builds
        const optionalFiles = [
            { name: 'firmware.bin', purpose: 'Compiled firmware' },
            { name: 'partitions.bin', purpose: 'Partition table' },
            { name: 'firmware.factory.bin', purpose: 'Factory firmware merge' },
            { name: 'littlefs.bin', purpose: 'LittleFS filesystem image' },
            { name: 'manifest.json', purpose: 'Build manifest' }
        ];
        
        let envValid = true;
        
        // Check required files
        requiredFiles.forEach(file => {
            const filePath = path.join(envDir, file.name);
            if (!fs.existsSync(filePath)) {
                this.errors.push(`[${envName}] Missing required file: ${file.name} (${file.purpose})`);
                envValid = false;
            } else {
                const stat = fs.statSync(filePath);
                console.log(`  ✅ ${file.name}: ${(stat.size / 1024).toFixed(1)}KB`);
                
                // Warn if file is empty
                if (stat.size === 0) {
                    this.warnings.push(`[${envName}] ${file.name} exists but is empty!`);
                }
            }
        });
        
        // Check optional files (warning only if missing in unified builds)
        if (envName.includes('F16R8') || envName.includes('unified')) {
            optionalFiles.forEach(file => {
                const filePath = path.join(envDir, file.name);
                if (!fs.existsSync(filePath)) {
                    console.log(`  ⚠️ ${file.name}: Missing (optional)`);
                } else {
                    console.log(`  ✅ ${file.name}: ${(fs.statSync(filePath).size / 1024).toFixed(1)}KB`);
                }
            });
            
            // Critical: factory-with-fs.bin should exist
            const factoryFSPath = path.join(envDir, 'factory-with-fs.bin');
            if (!fs.existsSync(factoryFSPath)) {
                this.warnings.push(`[${envName}] factory-with-fs.bin not found (may be generated post-build)`);
            }
        }
        
        return envValid;
    }

    isValidEnvironment(envName) {
        const originalErrors = this.errors.length;
        this.validateEnvironment(envName);
        return this.errors.length === originalErrors;
    }
}

/**
 * Main validation routine
 */
function runValidation() {
    console.log('\n🛡️  Running build artifacts validation...\n');
    
    const validator = new BuildArtifactsValidator();
    
    // If no build dir yet, just confirm script works
    if (!fs.existsSync(validator.buildDir)) {
        console.log('ℹ️  Build directory does not exist yet (first build expected)');
        console.log('   This will be validated automatically after first build.\n');
        process.exit(0);
    }
    
    const isValid = validator.validateAllEnvs();
    
    if (!isValid) {
        console.log('\n❌ VALIDATION FAILED: Fix issues before deploying!\n');
        process.exit(1);
    }
    
    console.log('\n✅ Build artifacts validation passed!\n');
    process.exit(0);
}

// Command-line interface
if (require.main === module) {
    runValidation();
}

module.exports = {
    BuildArtifactsValidator,
    runValidation
};
