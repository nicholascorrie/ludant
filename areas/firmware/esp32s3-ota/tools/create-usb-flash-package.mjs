#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const templatesDirectory = path.resolve(import.meta.dirname, '../usb-flash-package');

function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd,
    encoding: options.binary ? null : options.capture ? 'utf8' : undefined,
    stdio: options.capture ? ['ignore', 'pipe', 'pipe'] : 'inherit'
  });
  if (result.error) throw new Error(`Could not run ${command}: ${result.error.message}`);
  if (result.status !== 0) {
    const output = options.capture ? `${result.stdout ?? ''}${result.stderr ?? ''}`.trim() : '';
    throw new Error(`${command} failed${result.status == null ? '' : ` (exit ${result.status})`}${output ? `:\n${output}` : ''}`);
  }
  return result;
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function safeBuildFile(buildDirectory, relativePath) {
  if (typeof relativePath !== 'string' || !relativePath || path.isAbsolute(relativePath)) {
    throw new Error(`Invalid ESP-IDF flash image path: ${relativePath}`);
  }
  const absolutePath = path.resolve(buildDirectory, relativePath);
  if (!absolutePath.startsWith(`${path.resolve(buildDirectory)}${path.sep}`) || !fs.statSync(absolutePath, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`Missing or unsafe ESP-IDF flash image: ${relativePath}`);
  }
  return absolutePath;
}

function imageEntry(buildDirectory, offset, relativePath) {
  const sourcePath = safeBuildFile(buildDirectory, relativePath);
  const data = fs.readFileSync(sourcePath);
  return {
    offset,
    sourcePath,
    fileName: path.basename(relativePath),
    size: data.length,
    sha256: sha256(data)
  };
}

function parseFlashImages(buildDirectory) {
  const flasherPath = path.join(buildDirectory, 'flasher_args.json');
  if (!fs.existsSync(flasherPath)) throw new Error('ESP-IDF did not generate build/flasher_args.json');
  const flasher = readJson(flasherPath);
  const bootloader = flasher.bootloader;
  const required = ['partition-table', 'otadata', 'app'];
  if (!bootloader?.offset || !bootloader?.file) throw new Error('Generated flash map is missing its bootloader');
  for (const key of required) {
    if (!flasher[key]?.offset || !flasher[key]?.file) throw new Error(`Generated flash map is missing ${key}`);
  }

  const entries = [
    imageEntry(buildDirectory, bootloader.offset, bootloader.file),
    ...required.map((key) => imageEntry(buildDirectory, flasher[key].offset, flasher[key].file))
  ];
  const basenames = entries.map((entry) => entry.fileName);
  if (new Set(basenames).size !== basenames.length) throw new Error('Generated flash images contain duplicate filenames');
  return {
    chip: flasher.extra_esptool_args?.chip ?? 'esp32s3',
    flashSettings: flasher.flash_settings ?? {},
    entries
  };
}

function appVersion(data) {
  return data.subarray(0x30, 0x50).toString('utf8').split('\0', 1)[0];
}

export function createUsbFlashPackage({ buildDirectory, version, outputPath, otaPackagePath }) {
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error(`Invalid firmware version: ${version}`);
  const absoluteBuildDirectory = path.resolve(buildDirectory);
  const flashMap = parseFlashImages(absoluteBuildDirectory);
  if (flashMap.chip !== 'esp32s3') throw new Error(`USB package expected esp32s3, got ${flashMap.chip}`);

  const appEntry = flashMap.entries.find((entry) => entry.fileName === 'ludant_esp32s3_ota.bin');
  if (!appEntry) throw new Error('Generated flash map is missing the Ludant ESP32-S3 application image');
  const appData = fs.readFileSync(appEntry.sourcePath);
  if (appVersion(appData) !== version) {
    throw new Error(`USB application metadata is ${appVersion(appData) || '<empty>'}; expected ${version}`);
  }

  const otaDirectory = path.resolve(otaPackagePath);
  const otaManifest = readJson(path.join(otaDirectory, 'manifest.json'));
  const otaData = fs.readFileSync(path.join(otaDirectory, 'firmware.bin'));
  if (otaManifest.firmwareVersion !== version || sha256(otaData) !== sha256(appData)) {
    throw new Error(`USB application image does not match the signed BLE OTA package for ${version}`);
  }

  const absoluteOutputPath = path.resolve(outputPath);
  fs.mkdirSync(path.dirname(absoluteOutputPath), { recursive: true });
  const workDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'ludant-usb-flash-'));
  const stagingDirectory = path.join(workDirectory, 'package');
  const temporaryZip = path.join(workDirectory, `${version}-usb.zip`);
  fs.mkdirSync(path.join(stagingDirectory, 'images'), { recursive: true });
  try {
    const images = flashMap.entries.map((entry) => {
      const relativePath = `images/${entry.fileName}`;
      fs.copyFileSync(entry.sourcePath, path.join(stagingDirectory, relativePath));
      return {
        offset: entry.offset,
        path: relativePath,
        size: entry.size,
        sha256: entry.sha256
      };
    });
    const manifest = {
      formatVersion: 1,
      product: 'ludant-esp32s3',
      hardware: 'ESP32-S3',
      firmwareVersion: version,
      chip: flashMap.chip,
      flashSettings: flashMap.flashSettings,
      bootloader: images[0],
      projectImages: images.slice(1)
    };
    fs.writeFileSync(path.join(stagingDirectory, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);

    for (const name of ['flash.py', 'flash-macos.command', 'flash-windows.bat']) {
      fs.copyFileSync(path.join(templatesDirectory, name), path.join(stagingDirectory, name));
    }
    const readme = fs.readFileSync(path.join(templatesDirectory, 'README.md'), 'utf8')
      .replaceAll('{{VERSION}}', version);
    fs.writeFileSync(path.join(stagingDirectory, 'README.txt'), readme);

    const entries = fs.readdirSync(stagingDirectory).sort();
    run('zip', ['-X', '-r', temporaryZip, ...entries], { cwd: stagingDirectory });
    run('unzip', ['-t', temporaryZip], { capture: true });
    fs.renameSync(temporaryZip, absoluteOutputPath);
    return { outputPath: absoluteOutputPath, manifest };
  } finally {
    fs.rmSync(workDirectory, { recursive: true, force: true });
  }
}

export function readUsbFlashPackageManifest(zipPath) {
  const result = run('unzip', ['-p', path.resolve(zipPath), 'manifest.json'], { capture: true });
  return JSON.parse(result.stdout);
}

export function validateUsbFlashPackage(zipPath, expectedVersion) {
  const absoluteZipPath = path.resolve(zipPath);
  run('unzip', ['-t', absoluteZipPath], { capture: true });
  const manifest = readUsbFlashPackageManifest(absoluteZipPath);
  if (manifest.formatVersion !== 1 || manifest.product !== 'ludant-esp32s3' || manifest.hardware !== 'ESP32-S3' || manifest.chip !== 'esp32s3') {
    throw new Error(`Unsupported USB firmware package: ${absoluteZipPath}`);
  }
  if (expectedVersion && manifest.firmwareVersion !== expectedVersion) {
    throw new Error(`USB package version mismatch: expected ${expectedVersion}, found ${manifest.firmwareVersion}`);
  }
  const entries = [manifest.bootloader, ...(manifest.projectImages ?? [])];
  const names = entries.map((entry) => entry.path);
  if (entries.length !== 4 || new Set(names).size !== entries.length || !manifest.flashSettings?.flash_mode || !manifest.flashSettings?.flash_freq || !manifest.flashSettings?.flash_size) {
    throw new Error(`USB package has an incomplete flash map: ${absoluteZipPath}`);
  }
  for (const entry of entries) {
    if (typeof entry.path !== 'string' || entry.path.startsWith('/') || entry.path.includes('..') || !/^0x[\da-f]+$/i.test(entry.offset)) {
      throw new Error(`USB package has an unsafe flash image entry: ${entry.path}`);
    }
    const result = run('unzip', ['-p', absoluteZipPath, entry.path], { capture: true, binary: true });
    const imageData = result.stdout;
    if (imageData.length !== entry.size || sha256(imageData) !== entry.sha256) {
      throw new Error(`USB package image checksum mismatch: ${entry.path}`);
    }
  }
  return manifest;
}

function cliArguments(args) {
  const options = {};
  for (let index = 0; index < args.length; index += 1) {
    const key = args[index];
    if (!['--build-dir', '--version', '--output', '--ota-package'].includes(key)) {
      throw new Error(`Unknown argument: ${key}`);
    }
    const value = args[index + 1];
    if (!value || value.startsWith('--')) throw new Error(`${key} requires a value`);
    options[key] = value;
    index += 1;
  }
  for (const key of ['--build-dir', '--version', '--output', '--ota-package']) {
    if (!options[key]) throw new Error(`Missing required argument: ${key}`);
  }
  return {
    buildDirectory: options['--build-dir'],
    version: options['--version'],
    outputPath: options['--output'],
    otaPackagePath: options['--ota-package']
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    const result = createUsbFlashPackage(cliArguments(process.argv.slice(2)));
    console.log(`Created ${result.outputPath}`);
  } catch (error) {
    console.error(`USB package creation failed: ${error.message}`);
    process.exitCode = 1;
  }
}
