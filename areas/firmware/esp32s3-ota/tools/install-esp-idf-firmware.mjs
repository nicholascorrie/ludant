#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import readline from 'node:readline/promises';
import { inspectBootLog, selectCatalogRelease } from './install-esp-idf-firmware-utils.mjs';
import { assertUnlockedConfiguration, checkUsbFlashEfuses } from './unlocked-firmware-safety.mjs';

const root = path.resolve(import.meta.dirname, '../../../..');
const firmwareRoot = path.join(root, 'areas/firmware/esp32s3-ota');
const buildRoot = path.join(firmwareRoot, 'build');
const appRoot = path.join(root, 'apps/ui/ludant-ios/ludant-ios');
const catalogPath = path.join(appRoot, 'FirmwareCatalog.json');
const validatorPath = path.join(firmwareRoot, 'tools/validate-firmware-catalog.mjs');

function usage() {
  console.log(`This script installs open Ludant firmware over USB without enabling eFuse security features.

For normal firmware updates, connect to the controller in the Ludant iOS app
and use its signed BLE firmware update flow. Already encrypted or Secure-Boot
devices cannot be converted back to an unlocked state; do not try to install
this plaintext image on them.

Latest-release install: npm run firmware:install -- [--port <serial-port>] [--dry-run]

The default install resolves FirmwareCatalog.json's latest version and asset,
then prompts for an attached macOS USB serial port. --dry-run validates the
package but never connects to or writes to a device.`);
}

function parseArguments(args) {
  const options = { dryRun: false };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === '--help' || argument === '-h') {
      usage();
      process.exit(0);
    }
    if (argument === '--dry-run') {
      options.dryRun = true;
      continue;
    }
    if (argument === '--version' || argument === '--port') {
      const value = args[index + 1];
      if (!value || value.startsWith('--')) throw new Error(`${argument} requires a value`);
      options[argument === '--version' ? 'version' : 'port'] = value;
      index += 1;
      continue;
    }
    throw new Error(`Unknown argument: ${argument}`);
  }
  return options;
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function sha256(filePath) {
  return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? root,
    env: options.env ?? process.env,
    encoding: options.capture ? 'utf8' : undefined,
    stdio: options.capture ? ['ignore', 'pipe', 'pipe'] : 'inherit'
  });
  if (result.error) throw new Error(`Could not run ${command}: ${result.error.message}`);
  if (result.status !== 0) {
    const details = options.capture ? `${result.stdout ?? ''}${result.stderr ?? ''}`.trim() : '';
    throw new Error(`${command} ${args.join(' ')} failed${result.status == null ? '' : ` (exit ${result.status})`}${details ? `:\n${details}` : ''}`);
  }
  return result;
}

function validateLocalArtifacts(requestedVersion) {
  const catalog = readJson(catalogPath);
  const release = selectCatalogRelease(catalog, requestedVersion);
  const packagePath = path.resolve(appRoot, release.asset);
  if (!packagePath.startsWith(`${path.join(appRoot, 'FirmwareAssets')}${path.sep}`)) {
    throw new Error(`Catalog asset path is outside FirmwareAssets: ${release.asset}`);
  }
  const manifestPath = path.join(packagePath, 'manifest.json');
  const packageBinary = path.join(packagePath, 'firmware.bin');
  const buildBinary = path.join(buildRoot, 'ludant_esp32s3_ota.bin');
  const sdkconfigHeader = path.join(buildRoot, 'config/sdkconfig.h');
  const flasherArgsPath = path.join(buildRoot, 'flasher_args.json');
  const bootloaderBinary = path.join(buildRoot, 'bootloader/bootloader.bin');
  const bootloaderFlashArgs = path.join(buildRoot, 'flash_bootloader_args');
  const projectFlashArgs = path.join(buildRoot, 'flash_project_args');

  for (const filePath of [manifestPath, packageBinary, buildBinary, sdkconfigHeader,
    flasherArgsPath, bootloaderBinary, bootloaderFlashArgs, projectFlashArgs]) {
    if (!fs.existsSync(filePath)) throw new Error(`Required release/build file is missing: ${path.relative(root, filePath)}`);
  }

  run(process.execPath, [validatorPath], {
    env: { ...process.env, LUDANT_REQUIRE_ESPIDF: '1' }
  });

  const manifest = readJson(manifestPath);
  if (manifest.firmwareVersion !== release.version || manifest.runtime !== 'esp-idf') {
    throw new Error(`Selected package is not the catalogued ESP-IDF release ${release.version}`);
  }
  if (manifest.secureVersion !== undefined && manifest.secureVersion !== 0) {
    throw new Error('Selected package declares an eFuse anti-rollback version; install an unlocked release with secureVersion 0 or no secureVersion');
  }
  if (sha256(packageBinary) !== manifest.sha256 || sha256(buildBinary) !== manifest.sha256) {
    throw new Error(`The local ESP-IDF build does not match signed package ${release.version}. Rebuild that release before installing.`);
  }

  const config = fs.readFileSync(sdkconfigHeader, 'utf8');
  const requiredConfig = [
    `#define CONFIG_LUDANT_FIRMWARE_VERSION "${release.version}"`,
    '#define CONFIG_ESPTOOLPY_FLASHSIZE "4MB"'
  ];
  const missingConfig = requiredConfig.filter((line) => !config.split('\n').includes(line));
  if (missingConfig.length) throw new Error(`Build is not the expected production configuration: ${missingConfig.join(', ')}`);
  assertUnlockedConfiguration(config, 'built firmware');

  const flasherArgs = readJson(flasherArgsPath);
  const flashFiles = flasherArgs.flash_files ?? {};
  for (const [offset, relativePath] of Object.entries(flashFiles)) {
    const filePath = path.resolve(buildRoot, relativePath);
    if (!filePath.startsWith(`${buildRoot}${path.sep}`) || !fs.existsSync(filePath)) {
      throw new Error(`Invalid or missing flash image at ${offset}: ${relativePath}`);
    }
  }
  if (!Object.values(flashFiles).includes('partition_table/partition-table.bin') ||
      !Object.values(flashFiles).includes('ota_data_initial.bin') ||
      !Object.values(flashFiles).includes('ludant_esp32s3_ota.bin')) {
    throw new Error('Generated flash map is missing the partition table, OTA data, or application image');
  }

  return { release, bootloaderFlashArgs, projectFlashArgs };
}

function listSerialPorts() {
  return fs.readdirSync('/dev')
    .filter((name) => /^cu\.(?:usb|SLAB|wchusbserial|usbserial)/i.test(name))
    .map((name) => path.join('/dev', name))
    .sort();
}

async function selectPort(requestedPort) {
  const ports = listSerialPorts();
  if (requestedPort) {
    if (!fs.existsSync(requestedPort)) {
      throw new Error(`Serial port not found: ${requestedPort}${ports.length ? `\nAvailable USB serial ports:\n${ports.join('\n')}` : '\nReconnect the board and check its USB cable.'}`);
    }
    return requestedPort;
  }
  if (!ports.length) throw new Error('No USB serial ports found. Connect the ESP32-S3 with a data-capable USB cable and retry.');
  if (ports.length === 1) return ports[0];
  if (!process.stdin.isTTY) throw new Error(`Multiple serial ports found; rerun with --port <path>:\n${ports.join('\n')}`);

  const terminal = readline.createInterface({ input: process.stdin, output: process.stdout });
  try {
    console.log('USB serial ports:');
    ports.forEach((port, index) => console.log(`  ${index + 1}) ${port}`));
    const answer = await terminal.question('Choose the ESP32-S3 port number: ');
    const selected = Number(answer);
    if (!Number.isInteger(selected) || selected < 1 || selected > ports.length) throw new Error('Invalid port selection');
    return ports[selected - 1];
  } finally {
    terminal.close();
  }
}

function checkFlashSize(port) {
  const result = run('python', ['-m', 'esptool', '--chip', 'esp32s3', '-p', port, 'flash-id'], { capture: true });
  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`;
  console.log(output.trim());
  const detectedSize = output.match(/Detected flash size:\s*(\d+(?:\.\d+)?)\s*MB/i);
  if (!detectedSize || Number(detectedSize[1]) < 4) {
    throw new Error('Could not confirm at least 4 MB of flash. This release needs a 4 MB minimum partition layout; nothing was flashed.');
  }
  console.log(`Flash capacity is ${detectedSize[1]} MB; the firmware layout requires at least 4 MB.`);
}

function checkSecurityFuses(port) {
  const output = checkUsbFlashEfuses(port, { cwd: root });
  console.log(output.trim());
}

function verifyPostFlashBoot(port, expectedVersion, timeoutMs = 45_000) {
  return new Promise((resolve, reject) => {
    const monitor = spawn('idf.py', ['-p', port, 'monitor'], {
      cwd: firmwareRoot,
      env: process.env,
      stdio: ['pipe', 'pipe', 'pipe']
    });
    let output = '';
    let settled = false;
    let timeout;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      if (!monitor.killed) monitor.kill('SIGTERM');
      if (error) reject(error);
      else resolve();
    };
    const onData = (chunk) => {
      output = `${output}${chunk.toString('utf8')}`.slice(-32_000);
      const state = inspectBootLog(output, expectedVersion);
      if (state === 'ready') finish(null);
      else if (state === 'version-mismatch') finish(new Error(`Device booted an unexpected firmware version. Expected ${expectedVersion}. Recent serial output:\n${output.split(/\r?\n/).slice(-35).join('\n')}`));
      else if (state === 'crashed') finish(new Error(`Firmware boot verification detected a panic/reboot. Recent serial output:\n${output.split(/\r?\n/).slice(-35).join('\n')}`));
    };
    monitor.stdout.on('data', onData);
    monitor.stderr.on('data', onData);
    monitor.once('error', (error) => finish(new Error(`Could not start ESP-IDF serial monitor: ${error.message}`)));
    monitor.once('close', (code, signal) => {
      if (!settled) finish(new Error(`Serial monitor exited before BLE became ready (code=${code}, signal=${signal}). Recent serial output:\n${output.split(/\r?\n/).slice(-35).join('\n')}`));
    });
    timeout = setTimeout(() => finish(new Error(`Timed out after ${timeoutMs / 1000}s waiting for firmware ${expectedVersion} to become BLE-ready. The image was flashed, but boot/advertising was not verified. Recent serial output:\n${output.split(/\r?\n/).slice(-35).join('\n')}`)), timeoutMs);
  });
}

async function confirmInstall(version, port) {
  if (!process.stdin.isTTY) throw new Error('A terminal is required to confirm replacing the board firmware.');
  console.log(`\nAbout to replace the bootloader, partition table, OTA data, and app on ${port} with unlocked ESP-IDF ${version}.`);
  console.log('This release keeps ROM USB download mode available and does not enable Secure Boot, flash encryption, or eFuse anti-rollback.');
  const expected = `FLASH UNLOCKED FIRMWARE ${version} ${port}`;
  const terminal = readline.createInterface({ input: process.stdin, output: process.stdout });
  try {
    const answer = await terminal.question(`Type exactly:\n${expected}\n> `);
    if (answer !== expected) throw new Error('Confirmation did not match; no flash writes were made.');
  } finally {
    terminal.close();
  }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const { release, bootloaderFlashArgs, projectFlashArgs } = validateLocalArtifacts(options.version);
  console.log(`Selected release: ${release.version} (ESP-IDF, unlocked USB flashing)`);
  console.log(`Verified full-flash build and signed package: ${release.asset}`);

  if (options.dryRun) {
    console.log('Dry run complete. No device was queried or changed.');
    return;
  }

  const port = await selectPort(options.port);
  console.log(`Selected serial port: ${port}`);
  checkFlashSize(port);
  checkSecurityFuses(port);
  await confirmInstall(release.version, port);

  console.log('\nFlashing open bootloader (board will not be reset yet)...');
  run('python', [
    '-m', 'esptool', '--chip', 'esp32s3', '-p', port, '-b', '460800',
    '--before', 'default-reset', '--after', 'no-reset', '--no-stub',
    'write-flash', '@flash_bootloader_args'
  ], { cwd: buildRoot });

  console.log('\nFlashing partition table, OTA data, and application...');
  run('python', [
    '-m', 'esptool', '--chip', 'esp32s3', '-p', port, '-b', '460800',
    '--before', 'default-reset', '--after', 'hard-reset', '--no-stub',
    'write-flash', '@flash_project_args'
  ], { cwd: buildRoot });

  console.log(`\nFlash commands completed for ${release.version}; verifying first boot and BLE advertising over serial...`);
  await verifyPostFlashBoot(port, release.version);
  console.log(`Verified: firmware ${release.version} booted and logged BLE advertising as Ludant. Confirm it appears in the Ludant iOS scan before disconnecting USB.`);
}

main().catch((error) => {
  console.error(`\nInstaller stopped: ${error.message}`);
  process.exitCode = 1;
});
