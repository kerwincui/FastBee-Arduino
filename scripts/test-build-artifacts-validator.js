'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const { BuildArtifactsValidator, sha256 } = require('./test-build-artifacts');

const ENVIRONMENT = 'esp32c3-F4R0';
const HEAD_SHA = 'a'.repeat(40);
const FLASH_BYTES = 4 * 1024 * 1024;
const BOOTLOADER_OFFSET = 0x2000;
const PARTITIONS_OFFSET = 0x9000;
const APP_OFFSET = 0x18000;
const FS_OFFSET = 0x350000;
const FLASH_SIZE_NIBBLES = { 4: 0x20, 8: 0x30, 16: 0x40 };

function write(filePath, data) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, data);
}

function createEspImage(flashMB) {
    return Buffer.from([
        0xe9, 0x03, 0x02, FLASH_SIZE_NIBBLES[flashMB],
        0x00, 0x00, 0x00, 0x00
    ]);
}

function createFixture(options = {}) {
    const environment = options.environment || ENVIRONMENT;
    const flashMB = options.flashMB || 4;
    const bootloaderFlashMB = options.bootloaderFlashMB || flashMB;
    const firmwareFlashMB = options.firmwareFlashMB || flashMB;
    const flashBytes = flashMB * 1024 * 1024;
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-artifacts-'));
    const buildDir = path.join(root, '.pio', 'build', environment);
    const deployDir = path.join(root, 'dist', 'firmware', environment);
    const firmware = createEspImage(firmwareFlashMB);
    const bootloader = createEspImage(bootloaderFlashMB);
    const partitions = Buffer.from('current partitions');
    const filesystem = Buffer.from('current littlefs payload');
    const factory = Buffer.alloc(flashBytes, 0xff);
    bootloader.copy(factory, BOOTLOADER_OFFSET);
    partitions.copy(factory, PARTITIONS_OFFSET);
    firmware.copy(factory, APP_OFFSET);
    filesystem.copy(factory, FS_OFFSET);

    const files = { 'firmware.bin': firmware, 'bootloader.bin': bootloader, 'partitions.bin': partitions, 'littlefs.bin': filesystem };
    Object.entries(files).forEach(([name, content]) => {
        write(path.join(buildDir, name), content);
        write(path.join(deployDir, name), content);
    });
    const factoryPath = path.join(deployDir, 'factory-with-fs.bin');
    const manifestPath = path.join(deployDir, 'manifest.json');
    write(factoryPath, factory);
    write(manifestPath, `${JSON.stringify({
        environment,
        generatedAt: new Date().toISOString(),
        partitionTable: `partitions/${flashMB}MB.csv`,
        flashImages: [
            { offset: `0x${BOOTLOADER_OFFSET.toString(16)}`, file: 'bootloader.bin' },
            { offset: `0x${PARTITIONS_OFFSET.toString(16)}`, file: 'partitions.bin' },
            { offset: `0x${APP_OFFSET.toString(16)}`, file: 'firmware.bin' },
            { offset: `0x${FS_OFFSET.toString(16)}`, file: 'littlefs.bin' }
        ]
    }, null, 2)}\n`);
    return { root, deployDir, environment, factory, factoryPath, manifestPath };
}

function updateManifest(fixture, mutate) {
    const manifest = JSON.parse(fs.readFileSync(fixture.manifestPath, 'utf8'));
    mutate(manifest);
    write(fixture.manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
}

function createReleaseManifest(fixture, gitSha = HEAD_SHA.slice(0, 12)) {
    const releaseDir = path.join(fixture.root, 'dist', 'firmware', 'all-latest');
    const releaseName = `fastbee-${ENVIRONMENT}.bin`;
    const releasePath = path.join(releaseDir, releaseName);
    write(releasePath, fixture.factory);
    write(path.join(releaseDir, 'manifest.json'), `${JSON.stringify({
        generatedAt: new Date().toISOString(),
        gitSha,
        outputDir: 'dist/firmware/all-latest',
        environments: [{
            environment: ENVIRONMENT,
            chip: 'esp32c3',
            flashMB: 4,
            psramMB: 0,
            partition: 'partitions/4MB.csv',
            otaCapable: false,
            otaSlots: 1,
            signature: { signed: false, algorithm: 'none' },
            releaseFile: releaseName,
            files: { 'factory-with-fs.bin': releaseName },
            sizes: { [releaseName]: FLASH_BYTES },
            sha256: { [releaseName]: sha256(releasePath) }
        }]
    }, null, 2)}\n`);
}

function validator(root) {
    return new BuildArtifactsValidator({ rootDir: root, headSha: HEAD_SHA, gitDirty: false, quiet: true });
}

const roots = [];
try {
    const complete = createFixture();
    roots.push(complete.root);
    assert.strictEqual(validator(complete.root).validateCurrentBuild([ENVIRONMENT]), true,
        'all four manifest-defined flash segments must pass at non-default offsets');

    const corruptedBootloader = createFixture();
    roots.push(corruptedBootloader.root);
    const bootloaderFactory = Buffer.from(corruptedBootloader.factory);
    bootloaderFactory[BOOTLOADER_OFFSET] ^= 0xff;
    write(corruptedBootloader.factoryPath, bootloaderFactory);
    const bootloaderValidator = validator(corruptedBootloader.root);
    assert.strictEqual(bootloaderValidator.validateCurrentBuild([ENVIRONMENT]), false,
        'a corrupted bootloader segment in the merged image must fail');
    assert.ok(bootloaderValidator.errors.some((error) => error.includes('bootloader.bin')));

    const wrongBootloaderHeader = createFixture({
        environment: 'esp32-F8R0',
        flashMB: 8,
        bootloaderFlashMB: 4
    });
    roots.push(wrongBootloaderHeader.root);
    const bootloaderHeaderValidator = validator(wrongBootloaderHeader.root);
    assert.strictEqual(
        bootloaderHeaderValidator.validateCurrentBuild([wrongBootloaderHeader.environment]),
        false,
        'a self-consistent merged image with the wrong bootloader flash-size header must fail'
    );
    assert.ok(bootloaderHeaderValidator.errors.some((error) =>
        error.includes('bootloader.bin flash-size nibble') && error.includes('8MB')));

    const wrongFirmwareHeader = createFixture({
        environment: 'esp32-F8R0',
        flashMB: 8,
        firmwareFlashMB: 4
    });
    roots.push(wrongFirmwareHeader.root);
    const firmwareHeaderValidator = validator(wrongFirmwareHeader.root);
    assert.strictEqual(
        firmwareHeaderValidator.validateCurrentBuild([wrongFirmwareHeader.environment]),
        false,
        'a self-consistent merged image with the wrong OTA firmware flash-size header must fail'
    );
    assert.ok(firmwareHeaderValidator.errors.some((error) =>
        error.includes('firmware.bin flash-size nibble') && error.includes('8MB')));

    const corruptedPartitions = createFixture();
    roots.push(corruptedPartitions.root);
    const partitionsFactory = Buffer.from(corruptedPartitions.factory);
    partitionsFactory[PARTITIONS_OFFSET] ^= 0xff;
    write(corruptedPartitions.factoryPath, partitionsFactory);
    const partitionsValidator = validator(corruptedPartitions.root);
    assert.strictEqual(partitionsValidator.validateCurrentBuild([ENVIRONMENT]), false,
        'a corrupted partition table segment in the merged image must fail');
    assert.ok(partitionsValidator.errors.some((error) => error.includes('partitions.bin')));

    const missingBootloaderSegment = createFixture();
    roots.push(missingBootloaderSegment.root);
    updateManifest(missingBootloaderSegment, (manifest) => {
        manifest.flashImages = manifest.flashImages.filter((entry) => entry.file !== 'bootloader.bin');
    });
    const missingBootloaderValidator = validator(missingBootloaderSegment.root);
    assert.strictEqual(missingBootloaderValidator.validateCurrentBuild([ENVIRONMENT]), false,
        'a manifest missing the bootloader segment must fail');
    assert.ok(missingBootloaderValidator.errors.some((error) => error.includes('exactly one bootloader.bin')));

    const overlapping = createFixture();
    roots.push(overlapping.root);
    updateManifest(overlapping, (manifest) => {
        manifest.flashImages.find((entry) => entry.file === 'partitions.bin').offset =
            `0x${(BOOTLOADER_OFFSET + 1).toString(16)}`;
    });
    const overlappingValidator = validator(overlapping.root);
    assert.strictEqual(overlappingValidator.validateCurrentBuild([ENVIRONMENT]), false,
        'overlapping flash image segments must fail');
    assert.ok(overlappingValidator.errors.some((error) => error.includes('segments overlap')));

    const missing = createFixture();
    roots.push(missing.root);
    fs.rmSync(path.join(missing.deployDir, 'littlefs.bin'));
    const missingValidator = validator(missing.root);
    assert.strictEqual(missingValidator.validateCurrentBuild([ENVIRONMENT]), false,
        'a missing required artifact must fail');
    assert.ok(missingValidator.errors.some((error) => error.includes('littlefs.bin')));

    const release = createFixture();
    roots.push(release.root);
    createReleaseManifest(release);
    assert.strictEqual(validator(release.root).validateRelease([ENVIRONMENT]), true,
        'a complete release tied to current HEAD must pass');

    createReleaseManifest(release, 'b'.repeat(12));
    const staleValidator = validator(release.root);
    assert.strictEqual(staleValidator.validateRelease([ENVIRONMENT]), false,
        'a stale release gitSha must fail');
    assert.ok(staleValidator.errors.some((error) => error.includes('stale gitSha')));

    const emptyRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-artifacts-empty-'));
    roots.push(emptyRoot);
    assert.strictEqual(validator(emptyRoot).validateCurrentBuild([ENVIRONMENT]), false,
        'missing build and deployment directories must never pass silently');
} finally {
    roots.forEach((root) => fs.rmSync(root, { recursive: true, force: true }));
}

console.log('FastBee build artifact validator contract OK');
