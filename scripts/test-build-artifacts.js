'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT_DIR = path.resolve(__dirname, '..');
const SUPPORTED_ENVIRONMENTS = [
    'esp32-F4R0',
    'esp32-F8R0',
    'esp32-F8R8',
    'esp32c3-F4R0',
    'esp32c6-F4R0',
    'esp32c6-F8R0',
    'esp32s3-F8R0',
    'esp32s3-F8R8',
    'esp32s3-F16R8'
];
const REQUIRED_DEPLOY_FILES = [
    'firmware.bin',
    'bootloader.bin',
    'partitions.bin',
    'littlefs.bin',
    'factory-with-fs.bin',
    'manifest.json'
];
const REQUIRED_BUILD_FILES = ['firmware.bin', 'bootloader.bin', 'partitions.bin', 'littlefs.bin'];
const REQUIRED_FLASH_IMAGE_FILES = ['bootloader.bin', 'partitions.bin', 'firmware.bin', 'littlefs.bin'];
const ESP_FLASH_SIZE_NIBBLES = new Map([
    [4, 0x20],
    [8, 0x30],
    [16, 0x40]
]);
const BUILD_INPUT_PATHS = [
    'platformio.ini', 'partitions', 'include', 'src', 'lib', 'data', 'web-src', 'patches',
    'scripts/fastbee-artifacts.py', 'scripts/gzip-www.js'
];

function sha256(filePath) {
    return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
}

function parseEnvironment(environment) {
    const match = /^(esp32|esp32c3|esp32c6|esp32s3)-F(4|8|16)R(0|8)$/.exec(environment);
    if (!match) return null;
    return { chip: match[1], flashMB: Number(match[2]), psramMB: Number(match[3]) };
}

function parseOffset(value) {
    if (typeof value === 'number' && Number.isSafeInteger(value)) return value;
    if (typeof value !== 'string' || !/^(?:0x[0-9a-f]+|\d+)$/i.test(value.trim())) return null;
    const parsed = Number.parseInt(value, value.trim().toLowerCase().startsWith('0x') ? 16 : 10);
    return Number.isSafeInteger(parsed) ? parsed : null;
}

function validTimestamp(value) {
    if (typeof value !== 'string' || !value.trim()) return false;
    const timestamp = Date.parse(value);
    return Number.isFinite(timestamp) && timestamp <= Date.now() + 5 * 60 * 1000;
}

function isChildPath(parent, child) {
    const relativePath = path.relative(parent, child);
    return relativePath !== '' && !relativePath.startsWith(`..${path.sep}`) && relativePath !== '..' && !path.isAbsolute(relativePath);
}

class BuildArtifactsValidator {
    constructor(options = {}) {
        this.rootDir = path.resolve(options.rootDir || ROOT_DIR);
        const platformioBuildDir = process.env.PLATFORMIO_BUILD_DIR;
        this.buildDir = path.resolve(options.buildDir || (platformioBuildDir
            ? path.resolve(this.rootDir, platformioBuildDir)
            : path.join(this.rootDir, '.pio', 'build')));
        this.deployDir = path.resolve(options.deployDir || path.join(this.rootDir, 'dist', 'firmware'));
        this.quiet = Boolean(options.quiet);
        this.headShaOverride = Object.prototype.hasOwnProperty.call(options, 'headSha') ? options.headSha : undefined;
        this.gitDirtyOverride = Object.prototype.hasOwnProperty.call(options, 'gitDirty') ? options.gitDirty : undefined;
        this.errors = [];
        this.warnings = [];
        this.checked = 0;
        this.passed = 0;
    }

    reset() {
        this.errors = [];
        this.warnings = [];
        this.checked = 0;
        this.passed = 0;
    }

    log(message) {
        if (!this.quiet) console.log(message);
    }

    requireNonEmptyFile(filePath, label) {
        if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
            this.errors.push(`${label}: missing ${filePath}`);
            return false;
        }
        if (fs.statSync(filePath).size <= 0) {
            this.errors.push(`${label}: file is empty: ${filePath}`);
            return false;
        }
        return true;
    }

    readJson(filePath, label) {
        try {
            return JSON.parse(fs.readFileSync(filePath, 'utf8'));
        } catch (error) {
            this.errors.push(`${label}: invalid JSON (${error.message})`);
            return null;
        }
    }

    validateEmbeddedImage(environment, files, manifest, metadata) {
        const factory = fs.readFileSync(files['factory-with-fs.bin']);
        const flashBytes = metadata.flashMB * 1024 * 1024;
        if (factory.length !== flashBytes) {
            this.errors.push(`[${environment}] factory-with-fs.bin size ${factory.length} != ${flashBytes}`);
        }

        if (!Array.isArray(manifest.flashImages)) {
            this.errors.push(`[${environment}] manifest flashImages must be an array`);
            return;
        }

        const deployDir = path.dirname(files['factory-with-fs.bin']);
        const segments = [];
        const seenFiles = new Set();
        const seenOffsets = new Set();
        manifest.flashImages.forEach((entry, index) => {
            const label = `[${environment}] flashImages[${index}]`;
            if (!entry || typeof entry !== 'object' || Array.isArray(entry)) {
                this.errors.push(`${label} must be an object`);
                return;
            }

            const offset = parseOffset(entry.offset);
            if (offset === null || offset < 0) {
                this.errors.push(`${label} offset is invalid`);
                return;
            }
            if (typeof entry.file !== 'string' || !entry.file || path.basename(entry.file) !== entry.file) {
                this.errors.push(`${label} file must be a plain file name`);
                return;
            }
            if (seenFiles.has(entry.file)) {
                this.errors.push(`[${environment}] duplicate flash image file: ${entry.file}`);
                return;
            }
            if (seenOffsets.has(offset)) {
                this.errors.push(`[${environment}] duplicate flash image offset: 0x${offset.toString(16)}`);
                return;
            }
            seenFiles.add(entry.file);
            seenOffsets.add(offset);

            const imagePath = path.join(deployDir, entry.file);
            if (!this.requireNonEmptyFile(imagePath, label)) return;
            const image = fs.readFileSync(imagePath);
            const end = offset + image.length;
            segments.push({ file: entry.file, offset, end });
            if (end > factory.length) {
                this.errors.push(`[${environment}] ${entry.file} exceeds factory-with-fs.bin at 0x${offset.toString(16)}`);
                return;
            }
            if (!factory.subarray(offset, end).equals(image)) {
                this.errors.push(`[${environment}] factory-with-fs.bin does not contain ${entry.file} at 0x${offset.toString(16)}`);
            }
            if (entry.file === 'bootloader.bin' || entry.file === 'firmware.bin') {
                const expectedFlashSizeNibble = ESP_FLASH_SIZE_NIBBLES.get(metadata.flashMB);
                if (image.length < 4 || factory[offset] !== 0xe9) {
                    this.errors.push(`[${environment}] ${entry.file} at 0x${offset.toString(16)} has an invalid ESP image header`);
                } else if ((factory[offset + 3] & 0xf0) !== expectedFlashSizeNibble) {
                    const actualNibble = factory[offset + 3] & 0xf0;
                    this.errors.push(
                        `[${environment}] ${entry.file} flash-size nibble 0x${actualNibble.toString(16)} does not match ${metadata.flashMB}MB`
                    );
                }
            }
        });

        REQUIRED_FLASH_IMAGE_FILES.forEach((fileName) => {
            const count = manifest.flashImages.filter((entry) => entry && entry.file === fileName).length;
            if (count !== 1) {
                this.errors.push(`[${environment}] manifest must contain exactly one ${fileName} flash segment (found ${count})`);
            }
        });

        segments.sort((left, right) => left.offset - right.offset);
        for (let index = 1; index < segments.length; index += 1) {
            const previous = segments[index - 1];
            const current = segments[index];
            if (current.offset < previous.end) {
                this.errors.push(`[${environment}] flash image segments overlap: ${previous.file} and ${current.file}`);
            }
        }
    }

    validateEnvironment(environment, requireBuild = false) {
        const initialErrors = this.errors.length;
        const metadata = parseEnvironment(environment);
        this.checked += 1;
        this.log(`Checking ${environment}`);
        if (!metadata || !SUPPORTED_ENVIRONMENTS.includes(environment)) {
            this.errors.push(`[${environment}] unsupported build environment`);
            return false;
        }

        const deployEnvDir = path.join(this.deployDir, environment);
        if (!fs.existsSync(deployEnvDir) || !fs.statSync(deployEnvDir).isDirectory()) {
            this.errors.push(`[${environment}] deployment directory is missing: ${deployEnvDir}`);
            return false;
        }
        const deployFiles = Object.fromEntries(REQUIRED_DEPLOY_FILES.map((name) => [name, path.join(deployEnvDir, name)]));
        REQUIRED_DEPLOY_FILES.forEach((name) => this.requireNonEmptyFile(deployFiles[name], `[${environment}]`));
        if (this.errors.length !== initialErrors) return false;

        const manifest = this.readJson(deployFiles['manifest.json'], `[${environment}] manifest.json`);
        if (!manifest) return false;
        if (manifest.environment !== environment) this.errors.push(`[${environment}] manifest environment mismatch`);
        if (!validTimestamp(manifest.generatedAt)) this.errors.push(`[${environment}] manifest generatedAt is missing or invalid`);
        if (manifest.partitionTable !== `partitions/${metadata.flashMB}MB.csv`) {
            this.errors.push(`[${environment}] manifest partitionTable mismatch`);
        }
        this.validateEmbeddedImage(environment, deployFiles, manifest, metadata);

        if (requireBuild) {
            const buildEnvDir = path.join(this.buildDir, environment);
            if (!fs.existsSync(buildEnvDir) || !fs.statSync(buildEnvDir).isDirectory()) {
                this.errors.push(`[${environment}] current build directory is missing: ${buildEnvDir}`);
            } else {
                REQUIRED_BUILD_FILES.forEach((name) => {
                    const buildPath = path.join(buildEnvDir, name);
                    if (!this.requireNonEmptyFile(buildPath, `[${environment}] current build`)) return;
                    if (sha256(buildPath) !== sha256(deployFiles[name])) {
                        this.errors.push(`[${environment}] ${name} differs between current build ${buildEnvDir} and deployment ${deployEnvDir}`);
                    }
                });
            }
        }

        const valid = this.errors.length === initialErrors;
        if (valid) this.passed += 1;
        return valid;
    }

    currentHeadSha() {
        if (this.headShaOverride !== undefined) return this.headShaOverride;
        const result = spawnSync('git', ['rev-parse', 'HEAD'], {
            cwd: this.rootDir,
            encoding: 'utf8',
            windowsHide: true
        });
        if (result.error || result.status !== 0) return null;
        return result.stdout.trim();
    }

    dirtyBuildInputs() {
        if (this.gitDirtyOverride !== undefined) return Boolean(this.gitDirtyOverride);
        const result = spawnSync('git', ['status', '--porcelain=v1', '--untracked-files=all', '--', ...BUILD_INPUT_PATHS], {
            cwd: this.rootDir,
            encoding: 'utf8',
            windowsHide: true
        });
        if (result.error || result.status !== 0) {
            this.errors.push('[Manifest] unable to inspect Git build inputs');
            return true;
        }
        const changes = result.stdout.trim().split(/\r?\n/).filter(Boolean);
        if (changes.length > 0) {
            this.errors.push(`[Manifest] build inputs are dirty; release cannot be tied to HEAD (${changes.slice(0, 5).join(', ')})`);
            return true;
        }
        return false;
    }

    validateReleaseManifest(expectedEnvironments) {
        const manifestPath = path.join(this.deployDir, 'all-latest', 'manifest.json');
        if (!this.requireNonEmptyFile(manifestPath, '[Manifest]')) return false;
        const manifest = this.readJson(manifestPath, '[Manifest]');
        if (!manifest) return false;
        if (!validTimestamp(manifest.generatedAt)) this.errors.push('[Manifest] generatedAt is missing or invalid');

        const headSha = this.currentHeadSha();
        if (!headSha) {
            this.errors.push('[Manifest] unable to resolve current Git HEAD');
        } else if (typeof manifest.gitSha !== 'string' || manifest.gitSha.length < 7 || !headSha.startsWith(manifest.gitSha)) {
            this.errors.push(`[Manifest] stale gitSha ${manifest.gitSha || '<missing>'}; current HEAD is ${headSha}`);
        }
        this.dirtyBuildInputs();

        const entries = Array.isArray(manifest.environments) ? manifest.environments : [];
        const names = entries.map((entry) => entry && entry.environment).filter(Boolean);
        const uniqueNames = new Set(names);
        if (entries.length !== names.length) this.errors.push('[Manifest] every environment entry must be an object with a name');
        if (uniqueNames.size !== names.length) this.errors.push('[Manifest] duplicate environment entries are not allowed');
        expectedEnvironments.forEach((environment) => {
            if (!uniqueNames.has(environment)) this.errors.push(`[Manifest] missing environment entry: ${environment}`);
        });
        uniqueNames.forEach((environment) => {
            if (!expectedEnvironments.includes(environment)) this.errors.push(`[Manifest] unexpected environment entry: ${environment}`);
        });

        const releaseRoot = path.resolve(this.deployDir, 'all-latest');
        entries.forEach((entry) => {
            if (!entry || typeof entry.environment !== 'string') return;
            const metadata = parseEnvironment(entry.environment);
            if (!metadata || !SUPPORTED_ENVIRONMENTS.includes(entry.environment)) {
                this.errors.push(`[Manifest:${entry.environment}] invalid environment name`);
                return;
            }
            if (entry.chip !== metadata.chip || entry.flashMB !== metadata.flashMB ||
                entry.psramMB !== metadata.psramMB || entry.partition !== `partitions/${metadata.flashMB}MB.csv` ||
                entry.otaCapable !== (metadata.flashMB >= 8) || entry.otaSlots !== (metadata.flashMB >= 8 ? 2 : 1)) {
                this.errors.push(`[Manifest:${entry.environment}] hardware, partition, or OTA metadata mismatch`);
            }
            if (!entry.signature || typeof entry.signature.signed !== 'boolean' || typeof entry.signature.algorithm !== 'string') {
                this.errors.push(`[Manifest:${entry.environment}] signature metadata is missing`);
            }
            if (typeof entry.releaseFile !== 'string' || path.basename(entry.releaseFile) !== entry.releaseFile) {
                this.errors.push(`[Manifest:${entry.environment}] releaseFile must be a plain file name`);
                return;
            }
            if (!entry.files || entry.files['factory-with-fs.bin'] !== entry.releaseFile) {
                this.errors.push(`[Manifest:${entry.environment}] factory-with-fs.bin mapping is invalid`);
            }

            const releasePath = path.resolve(releaseRoot, entry.releaseFile);
            if (!isChildPath(releaseRoot, releasePath) || !this.requireNonEmptyFile(releasePath, `[Manifest:${entry.environment}]`)) return;
            const expectedBytes = metadata.flashMB * 1024 * 1024;
            const actualBytes = fs.statSync(releasePath).size;
            const declaredBytes = entry.sizes && entry.sizes[entry.releaseFile];
            if (actualBytes !== expectedBytes || declaredBytes !== actualBytes) {
                this.errors.push(`[Manifest:${entry.environment}] release artifact size mismatch`);
            }
            const actualHash = sha256(releasePath);
            const declaredHash = entry.sha256 && entry.sha256[entry.releaseFile];
            if (!/^[a-f0-9]{64}$/.test(declaredHash || '') || declaredHash !== actualHash) {
                this.errors.push(`[Manifest:${entry.environment}] SHA-256 mismatch`);
            }
            const sourceFactory = path.join(this.deployDir, entry.environment, 'factory-with-fs.bin');
            if (fs.existsSync(sourceFactory) && sha256(sourceFactory) !== actualHash) {
                this.errors.push(`[Manifest:${entry.environment}] release image differs from its per-environment factory image`);
            }
        });
        return this.errors.length === 0;
    }

    validateCurrentBuild(environments = SUPPORTED_ENVIRONMENTS) {
        this.reset();
        environments.forEach((environment) => this.validateEnvironment(environment, true));
        return this.errors.length === 0;
    }

    validateRelease(environments = SUPPORTED_ENVIRONMENTS) {
        this.reset();
        environments.forEach((environment) => this.validateEnvironment(environment, false));
        this.validateReleaseManifest(environments);
        return this.errors.length === 0;
    }

    validateAllEnvs(environments) {
        return this.validateRelease(environments || SUPPORTED_ENVIRONMENTS);
    }

    isValidEnvironment(environment) {
        const initialErrors = this.errors.length;
        this.validateEnvironment(environment, false);
        return this.errors.length === initialErrors;
    }

    printReport() {
        if (this.quiet && this.errors.length === 0) return;
        console.log(`Artifact validation: checked=${this.checked}, passed=${this.passed}, failed=${this.checked - this.passed}`);
        this.errors.forEach((error) => console.error(`ERROR: ${error}`));
        this.warnings.forEach((warning) => console.warn(`WARN: ${warning}`));
    }
}

function parseArguments(args) {
    const options = { currentBuild: false, quiet: false, environments: [] };
    for (let index = 0; index < args.length; index += 1) {
        const argument = args[index];
        if (argument === '--current-build') options.currentBuild = true;
        else if (argument === '--quiet') options.quiet = true;
        else if (argument.startsWith('--env=')) options.environments.push(argument.slice('--env='.length));
        else if (argument === '--env' && args[index + 1]) options.environments.push(args[++index]);
        else throw new Error(`Unknown argument: ${argument}`);
    }
    options.environments = uniqueEnvironmentList(options.environments);
    const invalid = options.environments.filter((environment) => !SUPPORTED_ENVIRONMENTS.includes(environment));
    if (invalid.length > 0) throw new Error(`Unknown environment(s): ${invalid.join(', ')}`);
    if (options.environments.length > 0 && !options.currentBuild) {
        throw new Error('--env requires --current-build; release validation always checks the complete matrix');
    }
    return options;
}

function uniqueEnvironmentList(environments) {
    return [...new Set(environments.flatMap((value) => value.split(',')).filter(Boolean))];
}

function runValidation(args = process.argv.slice(2), overrides = {}) {
    const cli = parseArguments(args);
    const validator = new BuildArtifactsValidator({ ...overrides, quiet: cli.quiet });
    const environments = cli.environments.length > 0 ? cli.environments : SUPPORTED_ENVIRONMENTS;
    const valid = cli.currentBuild
        ? validator.validateCurrentBuild(environments)
        : validator.validateRelease(environments);
    validator.printReport();
    return valid;
}

if (require.main === module) {
    try {
        process.exitCode = runValidation() ? 0 : 1;
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}

module.exports = {
    BuildArtifactsValidator,
    REQUIRED_BUILD_FILES,
    REQUIRED_DEPLOY_FILES,
    REQUIRED_FLASH_IMAGE_FILES,
    SUPPORTED_ENVIRONMENTS,
    parseArguments,
    parseEnvironment,
    runValidation,
    sha256
};
