#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { assertUnlockedConfiguration, disableIrreversibleSecurityOptions } from './unlocked-firmware-safety.mjs';
import { createUsbFlashPackage, validateUsbFlashPackage } from './create-usb-flash-package.mjs';
import { createLaunchpadFlashPackage, LAUNCHPAD_BASE_URL } from './create-launchpad-flash-package.mjs';
import { beginUsbFlashAssetTransaction } from './usb-flash-asset-transaction.mjs';

const root = path.resolve(import.meta.dirname, '../../../..');
const firmwareRoot = path.join(root, 'areas/firmware/esp32s3-ota');
const sdkconfigPath = path.join(firmwareRoot, 'sdkconfig');
const projectPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios.xcodeproj/project.pbxproj');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const assetsDirectory = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareAssets');
const launchpadDirectory = path.join(root, 'docs/launchpad');
const artifactTool = path.join(firmwareRoot, 'tools/create-firmware-artifact.mjs');

function usage() {
  console.error('Usage: npm run firmware:release -- <version> [--dry-run]');
  console.error('Required environment: LUDANT_OTA_SIGNING_PRIVATE_KEY or LUDANT_OTA_SIGNING_KEY_FILE');
  process.exit(2);
}

const releaseArguments = process.argv.slice(2);
const version = releaseArguments.shift();
const dryRun = process.argv.includes('--dry-run');
if (!version || !/^\d+\.\d+\.\d+$/.test(version) || releaseArguments.some((argument) => argument !== '--dry-run')) usage();
const packageName = `${version}.ludantfirmware`;
const packagePath = path.join(assetsDirectory, packageName);
const usbPackagePath = path.join(assetsDirectory, `${version}-usb.zip`);

if (fs.existsSync(packagePath)) throw new Error(`Release asset already exists: ${packagePath}`);
if (fs.existsSync(usbPackagePath)) throw new Error(`USB release asset already exists: ${usbPackagePath}`);
if (dryRun) {
  console.log(`Would build unlocked ESP-IDF ${version} with ROM USB download mode available`);
  process.exit(0);
}

function read(file) { return fs.readFileSync(file, 'utf8'); }
function writeConfig(original) {
  let config = disableIrreversibleSecurityOptions(original ?? 'CONFIG_IDF_TARGET="esp32s3"\n');
  const remove = (key) => {
    config = config.replace(new RegExp(`^(?:${key}=.*|# ${key} is not set)$\\n?`, 'gm'), '');
  };
  const replace = (key, value) => {
    remove(key);
    const line = `${key}=${value}`;
    config = `${config.trimEnd()}\n${line}\n`;
  };
  remove('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE');
  remove('CONFIG_APP_ANTI_ROLLBACK');
  remove('CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK');
  remove('CONFIG_BOOTLOADER_APP_SECURE_VERSION');
  remove('CONFIG_LUDANT_SECURE_VERSION');
  remove('CONFIG_FLASH_ENCRYPTION_ENABLED');
  remove('CONFIG_SECURE_BOOT');
  remove('CONFIG_SECURE_BOOT_V1_ENABLED');
  remove('CONFIG_SECURE_BOOT_V2_ENABLED');
  remove('CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES');
  remove('CONFIG_SECURE_FLASH_ENC_ENABLED');
  remove('CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT');
  remove('CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE');
  remove('CONFIG_SECURE_BOOT_SIGNING_KEY');
  remove('CONFIG_SECURE_DISABLE_ROM_DL_MODE');
  remove('CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE');
  remove('CONFIG_NVS_ENCRYPTION');
  remove('CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC');
  replace('CONFIG_LUDANT_FIRMWARE_VERSION', JSON.stringify(version));
  replace('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE', 'y');
  replace('CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP', 'y');
  replace('CONFIG_ESPTOOLPY_FLASHSIZE_4MB', 'y');
  replace('CONFIG_ESPTOOLPY_FLASHSIZE', JSON.stringify('4MB'));
  replace('CONFIG_PARTITION_TABLE_OFFSET', '0x10000');
  for (const key of [
    'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK', 'CONFIG_APP_ANTI_ROLLBACK',
    'CONFIG_SECURE_BOOT', 'CONFIG_SECURE_BOOT_V1_ENABLED', 'CONFIG_SECURE_BOOT_V2_ENABLED',
    'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES', 'CONFIG_SECURE_FLASH_ENC_ENABLED',
    'CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT', 'CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE',
    'CONFIG_NVS_ENCRYPTION', 'CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC',
    'CONFIG_SECURE_DISABLE_ROM_DL_MODE', 'CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE'
  ]) config = `${config.trimEnd()}\n# ${key} is not set\n`;
  replace('CONFIG_LUDANT_OTA_PUBLIC_KEY_DER_HEX', JSON.stringify(publicKeyDerHex));
  return config;
}

function verifyUnlockedConfiguration(configPath) {
  const config = read(configPath);
  const required = [
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y',
    'CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP=y',
    'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y',
    'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"',
    'CONFIG_PARTITION_TABLE_OFFSET=0x10000'
  ];
  const missing = required.filter((entry) => !config.split('\n').includes(entry));
  if (missing.length > 0) throw new Error(`unlocked firmware configuration is incomplete: ${missing.join(', ')}`);
  assertUnlockedConfiguration(config, 'sdkconfig');
}

function verifyUnlockedBuildHeader(headerPath) {
  assertUnlockedConfiguration(read(headerPath), 'generated build configuration');
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
  let updated = source;
  const escapedName = packageName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const fileRefPattern = new RegExp(`^\\s*([A-F0-9]{24}) \/\\* ${escapedName} \/\\*\\/ = \\{isa = PBXFileReference;`, 'm');
  let fileRefId = updated.match(fileRefPattern)?.[1];
  if (!fileRefId) {
    fileRefId = crypto.randomBytes(12).toString('hex').toUpperCase();
    updated = updated.replace('/* End PBXFileReference section */', `\t\t${fileRefId} /* ${packageName} */ = {isa = PBXFileReference; lastKnownFileType = folder; path = "ludant-ios/FirmwareAssets/${packageName}"; sourceTree = "<group>"; };\n/* End PBXFileReference section */`);
  }

  const buildFilePattern = new RegExp(`^\\s*([A-F0-9]{24}) \/\\* ${escapedName} in Resources \/\\*\\/ = \\{isa = PBXBuildFile;`, 'm');
  let buildFileId = updated.match(buildFilePattern)?.[1];
  if (!buildFileId) {
    buildFileId = crypto.randomBytes(12).toString('hex').toUpperCase();
    updated = updated.replace('/* End PBXBuildFile section */', `\t\t${buildFileId} /* ${packageName} in Resources */ = {isa = PBXBuildFile; fileRef = ${fileRefId} /* ${packageName} */; };\n/* End PBXBuildFile section */`);
  }

  const resourceMarker = `${buildFileId} /* ${packageName} in Resources */`;
  const resourcesStart = updated.indexOf('/* Begin PBXResourcesBuildPhase section */');
  const resourcesEnd = updated.indexOf('/* End PBXResourcesBuildPhase section */', resourcesStart);
  const resourcesSection = resourcesStart >= 0 && resourcesEnd > resourcesStart
    ? updated.slice(resourcesStart, resourcesEnd)
    : '';
  if (!resourcesSection.includes(resourceMarker)) {
    const resourcesFiles = /(\/\* Begin PBXResourcesBuildPhase section \*\/[\s\S]*?files = \(\n)/;
    if (!resourcesFiles.test(updated)) throw new Error('Could not find the app Resources phase in the Xcode project');
    updated = updated.replace(resourcesFiles, `$1\t\t\t\t${resourceMarker},\n`);
  }

  const groupMarker = `${fileRefId} /* ${packageName} */`;
  const groupPattern = /(792417D030529FE8005C5944 \/\* Recovered References \/\*\/ = \{[\s\S]*?children = \(\n)([\s\S]*?)(\n\s*\);)/;
  const groupMatch = updated.match(groupPattern);
  if (groupMatch && !groupMatch[2].includes(groupMarker)) {
    updated = updated.replace(groupPattern, `$1\t\t\t\t${groupMarker},$2$3`);
  }

  const exception = `\t\t\t\tFirmwareAssets/${packageName}/firmware.bin,\n\t\t\t\tFirmwareAssets/${packageName}/manifest.json,\n`;
  if (!updated.includes(`FirmwareAssets/${packageName}/firmware.bin`)) {
    if (!updated.includes('\t\t\t\tFirmwareCatalog.json,\n')) throw new Error('Could not locate the Xcode synchronized-resource exceptions');
    updated = updated.replace('\t\t\t\tFirmwareCatalog.json,\n', `${exception}\t\t\t\tFirmwareCatalog.json,\n`);
  }
  if (updated === source) throw new Error(`Could not add ${packageName} to the Xcode project`);
  return updated;
}

function removeUsbPackageReferences(source) {
  return source.split('\n').filter((line) => !line.includes('-usb.zip')).join('\n');
}

function addUsbPackageReference(source, packageName) {
  let updated = source;
  const fileRefId = crypto.randomBytes(12).toString('hex').toUpperCase();
  const buildFileId = crypto.randomBytes(12).toString('hex').toUpperCase();
  const fileRef = `\t\t${fileRefId} /* ${packageName} */ = {isa = PBXFileReference; lastKnownFileType = archive.zip; path = "ludant-ios/FirmwareAssets/${packageName}"; sourceTree = "<group>"; };\n`;
  const buildFile = `\t\t${buildFileId} /* ${packageName} in Resources */ = {isa = PBXBuildFile; fileRef = ${fileRefId} /* ${packageName} */; };\n`;
  updated = updated.replace('/* End PBXFileReference section */', `${fileRef}/* End PBXFileReference section */`);
  updated = updated.replace('/* End PBXBuildFile section */', `${buildFile}/* End PBXBuildFile section */`);

  const resourceMarker = `${buildFileId} /* ${packageName} in Resources */`;
  const resourcesStart = updated.indexOf('/* Begin PBXResourcesBuildPhase section */');
  const resourcesEnd = updated.indexOf('/* End PBXResourcesBuildPhase section */', resourcesStart);
  const resourcesSection = resourcesStart >= 0 && resourcesEnd > resourcesStart
    ? updated.slice(resourcesStart, resourcesEnd)
    : '';
  if (!resourcesSection.includes(resourceMarker)) {
    const resourcesFiles = /(\/\* Begin PBXResourcesBuildPhase section \*\/[\s\S]*?files = \(\n)/;
    if (!resourcesFiles.test(updated)) throw new Error('Could not find the app Resources phase for the USB firmware package');
    updated = updated.replace(resourcesFiles, `$1\t\t\t\t${resourceMarker},\n`);
  }

  const groupMarker = `${fileRefId} /* ${packageName} */`;
  const groupPattern = /(792417D030529FE8005C5944 \/\* Recovered References \/\*\/ = \{[\s\S]*?children = \(\n)([\s\S]*?)(\n\s*\);)/;
  if (!groupPattern.test(updated)) throw new Error('Could not locate the Xcode resource group for the USB firmware package');
  updated = updated.replace(groupPattern, `$1\t\t\t\t${groupMarker},$2$3`);

  const exceptionMarker = '\t\t\t\tFirmwareCatalog.json,\n';
  if (!updated.includes(exceptionMarker)) throw new Error('Could not locate the synchronized resource exceptions for the USB firmware package');
  updated = updated.replace(exceptionMarker, `\t\t\t\tFirmwareAssets/${packageName},\n${exceptionMarker}`);
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
const originalSdkconfig = fs.existsSync(sdkconfigPath) ? read(sdkconfigPath) : null;
const originalProject = read(projectPath);
const originalCatalog = read(catalogPath);
const buildDirectory = path.join(firmwareRoot, 'build');
let packageCreated = false;
let usbPackageCreated = false;
let releaseCommitted = false;
let usbAssetTransaction;
try {
  usbAssetTransaction = beginUsbFlashAssetTransaction(assetsDirectory, launchpadDirectory);

  // Sanitize even an ignored, pre-existing sdkconfig before any ESP-IDF command
  // can configure the project from it. Leave the sanitized config in place so
  // later direct `idf.py build` invocations remain owner-flashable too.
  fs.writeFileSync(sdkconfigPath, writeConfig(originalSdkconfig));
  const setTarget = spawnSync('idf.py', ['set-target', 'esp32s3'], { cwd: firmwareRoot, env: process.env, stdio: 'inherit' });
  if (setTarget.error || setTarget.status !== 0) throw new Error(`idf.py set-target failed: ${setTarget.error?.message ?? setTarget.status}`);
  const targetSdkconfig = fs.existsSync(sdkconfigPath) ? read(sdkconfigPath) : originalSdkconfig;
  fs.writeFileSync(sdkconfigPath, writeConfig(targetSdkconfig));
  const build = spawnSync('idf.py', ['build', `-DPROJECT_VER=${version}`], { cwd: firmwareRoot, env: process.env, stdio: 'inherit' });
  if (build.error || build.status !== 0) throw new Error(`idf.py build failed: ${build.error?.message ?? build.status}`);
  verifyUnlockedConfiguration(sdkconfigPath);
  verifyUnlockedBuildHeader(path.join(buildDirectory, 'config/sdkconfig.h'));
  const binaryPath = findApplicationBinary(buildDirectory);
  fs.mkdirSync(assetsDirectory, { recursive: true });
  const sign = spawnSync(process.execPath, [artifactTool, binaryPath, packagePath,
    '--version', version, '--runtime', 'esp-idf',
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

  createUsbFlashPackage({
    buildDirectory,
    version,
    outputPath: usbPackagePath,
    otaPackagePath: packagePath
  });
  usbPackageCreated = true;
  validateUsbFlashPackage(usbPackagePath, version);
  const launchpadPackage = createLaunchpadFlashPackage({
    usbPackagePath,
    version,
    outputDirectory: launchpadDirectory,
    publicBaseURL: LAUNCHPAD_BASE_URL
  });

  const catalog = JSON.parse(originalCatalog);
  const legacyVersions = catalog.versions.filter((release) => String(release.version).startsWith('arduino-'));
  if (legacyVersions.length > 0) {
    console.warn(`Retiring ${legacyVersions.length} legacy Arduino catalog entr${legacyVersions.length === 1 ? 'y' : 'ies'}`);
  }
  const historicalVersions = catalog.versions
    .filter((release) => !String(release.version).startsWith('arduino-'))
    .map(({ usbAsset, launchpadConfigURL, ...release }) => release);
  const usbAsset = `FirmwareAssets/${path.basename(usbPackagePath)}`;
  const launchpadConfigURL = launchpadPackage.configURL;
  const updatedCatalog = {
    ...catalog,
    latest: { version, asset: `FirmwareAssets/${packageName}`, usbAsset, launchpadConfigURL },
    versions: [
      ...historicalVersions,
      { version, asset: `FirmwareAssets/${packageName}`, usbAsset, launchpadConfigURL }
    ]
  };
  fs.writeFileSync(catalogPath, `${JSON.stringify(updatedCatalog, null, 2)}\n`);
  const projectWithLatestArtifacts = addUsbPackageReference(
    addPackageReference(removeUsbPackageReferences(originalProject), packageName),
    path.basename(usbPackagePath)
  );
  fs.writeFileSync(projectPath, injectIOSPublicKey(projectWithLatestArtifacts));
  const validate = spawnSync(process.execPath, [path.join(firmwareRoot, 'tools/validate-firmware-catalog.mjs')], {
    cwd: root,
    env: { ...process.env, LUDANT_REQUIRE_ESPIDF: '1' },
    stdio: 'inherit'
  });
  if (validate.error || validate.status !== 0) throw new Error(`catalog validation failed: ${validate.error?.message ?? validate.status}`);
  usbAssetTransaction.commit();
  releaseCommitted = true;
  console.log(`Created ${path.relative(root, packagePath)}`);
  console.log(`Created ${path.relative(root, usbPackagePath)}`);
  console.log(`Created ${path.relative(root, launchpadDirectory)} (${launchpadPackage.manifest.image})`);
} catch (error) {
  if (packageCreated) fs.rmSync(packagePath, { recursive: true, force: true });
  if (usbPackageCreated) fs.rmSync(usbPackagePath, { force: true });
  fs.writeFileSync(projectPath, originalProject);
  fs.writeFileSync(catalogPath, originalCatalog);
  usbAssetTransaction?.rollback();
  throw error;
} finally {
  if (!releaseCommitted) usbAssetTransaction?.rollback();
  const currentSdkconfig = fs.existsSync(sdkconfigPath) ? read(sdkconfigPath) : originalSdkconfig;
  fs.writeFileSync(sdkconfigPath, writeConfig(currentSdkconfig));
}
