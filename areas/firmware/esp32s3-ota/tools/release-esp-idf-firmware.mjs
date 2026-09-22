#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../../../..');
const firmwareRoot = path.join(root, 'areas/firmware/esp32s3-ota');
const sdkconfigPath = path.join(firmwareRoot, 'sdkconfig');
const projectPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios.xcodeproj/project.pbxproj');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const assetsDirectory = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareAssets');
const artifactTool = path.join(firmwareRoot, 'tools/create-firmware-artifact.mjs');

function usage() {
  console.error('Usage: npm run firmware:release -- <version> <secure-version> [--dry-run]');
  console.error('Required environment: LUDANT_OTA_SIGNING_PRIVATE_KEY or LUDANT_OTA_SIGNING_KEY_FILE');
  console.error('Production build also requires LUDANT_SECURE_BOOT_SIGNING_KEY_FILE outside the repository');
  process.exit(2);
}

const [version, secureVersionText] = process.argv.slice(2);
const dryRun = process.argv.includes('--dry-run');
if (!version || !/^\d+\.\d+\.\d+$/.test(version) || !/^\d+$/.test(secureVersionText ?? '')) usage();
const secureVersion = Number(secureVersionText);
const packageName = `${version}.ludantfirmware`;
const packagePath = path.join(assetsDirectory, packageName);

function read(file) { return fs.readFileSync(file, 'utf8'); }
function writeConfig(original) {
  let config = original ?? 'CONFIG_IDF_TARGET="esp32s3"\n';
  const replace = (key, value) => {
    const expression = new RegExp(`^${key}=.*$`, 'm');
    const line = `${key}=${value}`;
    config = expression.test(config) ? config.replace(expression, line) : `${config.trimEnd()}\n${line}\n`;
  };
  replace('CONFIG_LUDANT_FIRMWARE_VERSION', JSON.stringify(version));
  replace('CONFIG_LUDANT_SECURE_VERSION', String(secureVersion));
  replace('CONFIG_BOOTLOADER_APP_SECURE_VERSION', String(secureVersion));
  replace('CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK', 'y');
  replace('CONFIG_APP_ANTI_ROLLBACK', 'y');
  replace('CONFIG_SECURE_BOOT_V2_ENABLED', 'y');
  replace('CONFIG_FLASH_ENCRYPTION_ENABLED', 'y');
  replace('CONFIG_NVS_ENCRYPTION', 'y');
  replace('CONFIG_LUDANT_OTA_PUBLIC_KEY_DER_HEX', JSON.stringify(publicKeyDerHex));
  replace('CONFIG_SECURE_BOOT_SIGNING_KEY', JSON.stringify(secureBootKeyPath));
  return config;
}

function verifyProductionSecurityConfiguration(configPath) {
  const config = read(configPath);
  const required = [
    'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y',
    'CONFIG_APP_ANTI_ROLLBACK=y',
    'CONFIG_SECURE_BOOT_V2_ENABLED=y',
    'CONFIG_FLASH_ENCRYPTION_ENABLED=y',
    'CONFIG_NVS_ENCRYPTION=y',
    `CONFIG_SECURE_BOOT_SIGNING_KEY="${secureBootKeyPath}"`,
    `CONFIG_BOOTLOADER_APP_SECURE_VERSION=${secureVersion}`
  ];
  const missing = required.filter((entry) => !config.split('\n').includes(entry));
  if (missing.length > 0) throw new Error(`production security configuration is incomplete: ${missing.join(', ')}`);
}

function findApplicationBinary(buildDirectory) {
  const preferred = path.join(buildDirectory, 'ludant_esp32s3_ota.bin');
  if (fs.existsSync(preferred)) return preferred;
  const binaries = [];
  const visit = (directory) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const entryPath = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(entryPath);
      else if (entry.isFile() && entry.name.endsWith('.bin') && !/(bootloader|partitions|merged|ota_data|boot_app0)/i.test(entry.name)) binaries.push(entryPath);
    }
  };
  visit(buildDirectory);
  if (binaries.length !== 1) throw new Error(`Expected exactly one ESP-IDF application binary, found ${binaries.length}`);
  return binaries[0];
}

function addPackageReference(source, packageName) {
  const packageMarker = `/* ${packageName} */`;
  if (source.includes(packageMarker)) return source;
  const fileRefId = crypto.randomBytes(12).toString('hex').toUpperCase();
  const buildFileId = crypto.randomBytes(12).toString('hex').toUpperCase();
  let updated = source;
  updated = updated.replace('/* End PBXBuildFile section */', `\t\t${buildFileId} /* ${packageName} in Resources */ = {isa = PBXBuildFile; fileRef = ${fileRefId} /* ${packageName} */; };\n/* End PBXBuildFile section */`);
  updated = updated.replace('/* End PBXFileReference section */', `\t\t${fileRefId} /* ${packageName} */ = {isa = PBXFileReference; lastKnownFileType = folder; path = "ludant-ios/FirmwareAssets/${packageName}"; sourceTree = "<group>"; };\n/* End PBXFileReference section */`);
  updated = updated.replace(/(000000000000000140000000 \/\* Resources \/\*\/ = \{[\s\S]*?files = \(\n)/, `$1\t\t\t\t${buildFileId} /* ${packageName} in Resources */,\n`);
  updated = updated.replace(/(792417D030529FE8005C5944 \/\* Recovered References \/\*\/ = \{[\s\S]*?children = \(\n)/, `$1\t\t\t\t${fileRefId} /* ${packageName} */,\n`);
  const exception = `\t\t\t\tFirmwareAssets/${packageName}/firmware.bin,\n\t\t\t\tFirmwareAssets/${packageName}/manifest.json,\n`;
  updated = updated.replace('\t\t\t\tFirmwareCatalog.json,\n', `${exception}\t\t\t\tFirmwareCatalog.json,\n`);
  if (updated === source) throw new Error(`Could not add ${packageName} to the Xcode project`);
  return updated;
}

function injectIOSPublicKey(source) {
  const expression = /LUDANT_OTA_PUBLIC_KEY = "[^"]*";/g;
  if (!expression.test(source)) throw new Error('Could not find the iOS OTA public key build setting');
  return source.replace(expression, `LUDANT_OTA_PUBLIC_KEY = "${publicKeyRawBase64}";`);
}

const privateKey = process.env.LUDANT_OTA_SIGNING_PRIVATE_KEY ??
  (process.env.LUDANT_OTA_SIGNING_KEY_FILE && read(path.resolve(process.env.LUDANT_OTA_SIGNING_KEY_FILE)));
if (!privateKey) throw new Error('Set LUDANT_OTA_SIGNING_PRIVATE_KEY or LUDANT_OTA_SIGNING_KEY_FILE');
const privateKeyObject = crypto.createPrivateKey(privateKey);
if (privateKeyObject.asymmetricKeyType !== 'ed25519') throw new Error('OTA signing key must be Ed25519');
const publicKeyDer = crypto.createPublicKey(privateKeyObject).export({ format: 'der', type: 'spki' });
const publicKeyRaw = publicKeyDer.subarray(publicKeyDer.length - 32);
const publicKeyDerHex = publicKeyDer.toString('hex');
const publicKeyRawBase64 = publicKeyRaw.toString('base64');
const secureBootKeyPath = process.env.LUDANT_SECURE_BOOT_SIGNING_KEY_FILE &&
  path.resolve(process.env.LUDANT_SECURE_BOOT_SIGNING_KEY_FILE);
if (!secureBootKeyPath || !fs.existsSync(secureBootKeyPath)) {
  throw new Error('LUDANT_SECURE_BOOT_SIGNING_KEY_FILE must point to an existing production key outside the repository');
}
const relativeSecureBootKey = path.relative(root, secureBootKeyPath);
if (relativeSecureBootKey === '' || (!relativeSecureBootKey.startsWith('..') && !path.isAbsolute(relativeSecureBootKey))) {
  throw new Error('LUDANT_SECURE_BOOT_SIGNING_KEY_FILE must point outside the repository');
}
if (fs.existsSync(packagePath)) throw new Error(`Release asset already exists: ${packagePath}`);
if (dryRun) {
  console.log(`Would build ESP-IDF ${version} with secure version ${secureVersion}`);
  process.exit(0);
}

const originalSdkconfig = fs.existsSync(sdkconfigPath) ? read(sdkconfigPath) : null;
const originalProject = read(projectPath);
const originalCatalog = read(catalogPath);
const buildDirectory = path.join(firmwareRoot, 'build');
let packageCreated = false;
try {
  fs.writeFileSync(sdkconfigPath, writeConfig(originalSdkconfig));
  const setTarget = spawnSync('idf.py', ['set-target', 'esp32s3'], { cwd: firmwareRoot, env: process.env, stdio: 'inherit' });
  if (setTarget.error || setTarget.status !== 0) throw new Error(`idf.py set-target failed: ${setTarget.error?.message ?? setTarget.status}`);
  const build = spawnSync('idf.py', ['build', `-DPROJECT_VER=${version}`], { cwd: firmwareRoot, env: process.env, stdio: 'inherit' });
  if (build.error || build.status !== 0) throw new Error(`idf.py build failed: ${build.error?.message ?? build.status}`);
  verifyProductionSecurityConfiguration(sdkconfigPath);
  const binaryPath = findApplicationBinary(buildDirectory);
  fs.mkdirSync(assetsDirectory, { recursive: true });
  const sign = spawnSync(process.execPath, [artifactTool, binaryPath, packagePath,
    '--version', version, '--runtime', 'esp-idf', '--secure-version', String(secureVersion),
    '--minimum-partition-size', '1310720', '--module-implementations', JSON.stringify({
      'sensor.mpu6050': '1.0.0', 'sensor.bme280': '1.0.0', 'sensor.soil-moisture': '1.0.0', 'actuator.relay': '1.0.0'
    }), '--module-schemas', JSON.stringify({
      'sensor.mpu6050': 1, 'sensor.bme280': 1, 'sensor.soil-moisture': 1, 'actuator.relay': 1
    })], {
    cwd: root,
    env: { ...process.env, LUDANT_OTA_SIGNING_PRIVATE_KEY: privateKey },
    stdio: 'inherit'
  });
  if (sign.error || sign.status !== 0) throw new Error(`artifact signing failed: ${sign.error?.message ?? sign.status}`);
  packageCreated = true;
  const catalog = JSON.parse(originalCatalog);
  const updatedCatalog = { ...catalog, versions: [{ version, asset: `FirmwareAssets/${packageName}` }] };
  fs.writeFileSync(catalogPath, `${JSON.stringify(updatedCatalog, null, 2)}\n`);
  fs.writeFileSync(projectPath, injectIOSPublicKey(addPackageReference(originalProject, packageName)));
  const validate = spawnSync(process.execPath, [path.join(firmwareRoot, 'tools/validate-firmware-catalog.mjs')], {
    cwd: root,
    env: { ...process.env, LUDANT_REQUIRE_ESPIDF: '1' },
    stdio: 'inherit'
  });
  if (validate.error || validate.status !== 0) throw new Error(`catalog validation failed: ${validate.error?.message ?? validate.status}`);
  console.log(`Created ${path.relative(root, packagePath)}`);
} catch (error) {
  if (packageCreated) fs.rmSync(packagePath, { recursive: true, force: true });
  fs.writeFileSync(projectPath, originalProject);
  fs.writeFileSync(catalogPath, originalCatalog);
  throw error;
} finally {
  if (originalSdkconfig == null) fs.rmSync(sdkconfigPath, { force: true });
  else fs.writeFileSync(sdkconfigPath, originalSdkconfig);
}
