#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../../../..');
const sketchDirectory = path.join(root, 'areas/firmware/esp32s3-ota/arduino/ludant_esp32s3_ota');
const versionHeaderPath = path.join(sketchDirectory, 'firmware_version.hpp');
const authHeaderPath = path.join(root, 'areas/firmware/esp32s3-ota/shared/ota_auth.hpp');
const projectPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios.xcodeproj/project.pbxproj');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const assetsDirectory = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareAssets');
const releaseToolPath = path.join(root, 'areas/firmware/esp32s3-ota/tools/create-firmware-artifact.mjs');
const maximumPartitionSize = '1310720';

function usage() {
  console.error('Usage: npm run firmware:release -- <major|minor|patch> [--dry-run]');
  console.error('Required environment: LUDANT_OTA_SIGNING_PRIVATE_KEY or LUDANT_OTA_SIGNING_KEY_FILE');
  process.exit(2);
}

const bump = process.argv[2];
const dryRun = process.argv.includes('--dry-run');
if (!['major', 'minor', 'patch'].includes(bump)) usage();

function read(pathname) {
  return fs.readFileSync(pathname, 'utf8');
}

function parseVersion(value) {
  const match = value.match(/^(.*-)?(\d+)\.(\d+)\.(\d+)$/);
  if (!match) throw new Error(`Firmware version must end in major.minor.patch: ${value}`);
  return { prefix: match[1] ?? '', major: Number(match[2]), minor: Number(match[3]), patch: Number(match[4]) };
}

function formatVersion(version) {
  return `${version.prefix}${version.major}.${version.minor}.${version.patch}`;
}

function incrementVersion(current, part) {
  const next = parseVersion(current);
  if (part === 'major') {
    next.major += 1;
    next.minor = 0;
    next.patch = 0;
  } else if (part === 'minor') {
    next.minor += 1;
    next.patch = 0;
  } else {
    next.patch += 1;
  }
  return formatVersion(next);
}

function compareVersions(left, right) {
  const a = parseVersion(left);
  const b = parseVersion(right);
  return a.major - b.major || a.minor - b.minor || a.patch - b.patch;
}

function findApplicationBinary(directory) {
  const binaries = [];
  function visit(current) {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const entryPath = path.join(current, entry.name);
      if (entry.isDirectory()) visit(entryPath);
      else if (entry.isFile() && entry.name.endsWith('.bin') &&
               !/(bootloader|partitions|merged|ota_data|boot_app0)/i.test(entry.name)) {
        binaries.push(entryPath);
      }
    }
  }
  visit(directory);
  if (binaries.length !== 1) {
    throw new Error(`Expected exactly one application .bin, found ${binaries.length}: ${binaries.join(', ')}`);
  }
  return binaries[0];
}

function signingKey() {
  const configuredKeyPath = process.env.LUDANT_OTA_SIGNING_KEY_FILE;
  const localKeyPath = path.join(os.homedir(), '.config/ludant/ota-ed25519-private.pem');
  const keyPath = configuredKeyPath ? path.resolve(configuredKeyPath) :
    (fs.existsSync(localKeyPath) ? localKeyPath : undefined);
  const pem = process.env.LUDANT_OTA_SIGNING_PRIVATE_KEY ?? (keyPath ? read(keyPath) : undefined);
  if (!pem) throw new Error('Set LUDANT_OTA_SIGNING_PRIVATE_KEY or LUDANT_OTA_SIGNING_KEY_FILE');
  const privateKey = crypto.createPrivateKey(pem);
  if (privateKey.asymmetricKeyType !== 'ed25519') throw new Error('The signing key must be Ed25519');
  const publicKeyDer = crypto.createPublicKey(privateKey).export({ format: 'der', type: 'spki' });
  return {
    pem,
    publicKeyDerHex: publicKeyDer.toString('hex'),
    publicKeyRawBase64: publicKeyDer.subarray(publicKeyDer.length - 32).toString('base64')
  };
}

function replaceExactly(source, expression, replacement, description) {
  const matches = source.match(expression);
  if (!matches || matches.length !== 1) throw new Error(`Could not uniquely update ${description}`);
  return source.replace(expression, replacement);
}

function removeFirmwareAssetReferences(source, packageNames) {
  // Firmware packages are folder references. If the synchronized app group
  // also includes FirmwareAssets, Xcode copies each package's children into
  // the app root and collides on names such as manifest.json and firmware.bin.
  // Keep only one explicit package reference in the resources phase.
  let updated = source
    .replace(/^[ \t]*[A-F0-9]+ \/\* [^*]*\.ludantfirmware in Resources \*\/ = .*\n/gm, '')
    .replace(/^[ \t]*[A-F0-9]+ \/\* [^*]*\.ludantfirmware \*\/ = \{isa = PBXFileReference;[^\n]*\n/gm, '')
    .replace(/^[ \t]*[A-F0-9]+ \/\* [^*]*\.ludantfirmware in Resources \*\/,\n/gm, '')
    .replace(/^[ \t]*[A-F0-9]+ \/\* [^*]*\.ludantfirmware \*\/,\n/gm, '');

  // Xcode requires the package paths themselves in the exception set; an
  // exception for the parent FirmwareAssets directory is not sufficient.
  updated = updated
    .replace(/^[ \t]*FirmwareAssets,\n/gm, '')
    .replace(/^[ \t]*FirmwareAssets\/[^,\n]+\.ludantfirmware,\n/gm, '');
  const exceptionMarker = '\t\t\t\tFirmwareCatalog.json,\n';
  if (!updated.includes(exceptionMarker)) {
    throw new Error('Could not find the synchronized app resource exceptions');
  }
  const exceptions = [...packageNames]
    .sort()
    .map((name) => `\t\t\t\tFirmwareAssets/${name}/firmware.bin,\n\t\t\t\tFirmwareAssets/${name}/manifest.json,\n`)
    .filter((line) => !updated.includes(line))
    .join('');
  if (exceptions) {
    updated = updated.replace(exceptionMarker, `${exceptions}${exceptionMarker}`);
  }
  return updated;
}

function addFirmwareAssetToXcodeProject(source, packageName) {
  if (source.includes(`/* ${packageName} in Resources */`)) return source;
  const fileRefId = crypto.randomBytes(12).toString('hex').toUpperCase();
  const buildFileId = crypto.randomBytes(12).toString('hex').toUpperCase();
  const buildFile = `\t\t${buildFileId} /* ${packageName} in Resources */ = {isa = PBXBuildFile; fileRef = ${fileRefId} /* ${packageName} */; };\n`;
  const fileRef = `\t\t${fileRefId} /* ${packageName} */ = {isa = PBXFileReference; lastKnownFileType = folder; path = "ludant-ios/FirmwareAssets/${packageName}"; sourceTree = "<group>"; };\n`;
  const resourceBuild = `\t\t\t\t${buildFileId} /* ${packageName} in Resources */,\n`;
  let updated = source;
  updated = updated.replace('/* End PBXBuildFile section */', `${buildFile}/* End PBXBuildFile section */`);
  updated = updated.replace('/* End PBXFileReference section */', `${fileRef}/* End PBXFileReference section */`);
  updated = updated.replace(
    /000000000000000140000000 \/\* Resources \*\/ = \{([\s\S]*?files = \(\n)/,
    (match) => `${match}${resourceBuild}`
  );
  updated = updated.replace(
    /(792417D030529FE8005C5944 \/\* Recovered References \*\/ = \{[\s\S]*?children = \(\n)/,
    (match) => `${match}\t\t\t\t${fileRefId} /* ${packageName} */,\n`
  );
  if (updated === source) throw new Error(`Could not add ${packageName} to the Xcode resources`);
  return updated;
}

const originalVersionHeader = read(versionHeaderPath);
const originalAuthHeader = read(authHeaderPath);
const originalProject = read(projectPath);
const originalCatalog = read(catalogPath);
const versionMatch = originalVersionHeader.match(/#define LUDANT_FIRMWARE_VERSION "([^"]+)"/);
if (!versionMatch) throw new Error('Could not find LUDANT_FIRMWARE_VERSION in the Arduino version header');
const currentVersion = versionMatch[1];
const nextVersion = incrementVersion(currentVersion, bump);
const packageName = `${nextVersion}.ludantfirmware`;
const packagePath = path.join(assetsDirectory, packageName);

const catalog = JSON.parse(originalCatalog);
if (!Array.isArray(catalog.versions)) throw new Error('FirmwareCatalog.json must contain a versions array');
if (catalog.versions.some((entry) => entry.version === nextVersion)) {
  throw new Error(`Catalog already contains ${nextVersion}`);
}
if (fs.existsSync(packagePath)) throw new Error(`Release asset already exists: ${packagePath}`);

console.log(`Current firmware: ${currentVersion}`);
console.log(`Next firmware:    ${nextVersion}`);
console.log(`Asset:            ${path.relative(root, packagePath)}`);
if (dryRun) process.exit(0);

const key = signingKey();
const updatedVersionHeader = replaceExactly(
  originalVersionHeader,
  /#define LUDANT_FIRMWARE_VERSION "[^"]+"/,
  `#define LUDANT_FIRMWARE_VERSION "${nextVersion}"`,
  'the Arduino firmware version'
);
const updatedAuthHeader = replaceExactly(
  originalAuthHeader,
  /#define LUDANT_OTA_PUBLIC_KEY_DER_HEX "[^"]*"/,
  `#define LUDANT_OTA_PUBLIC_KEY_DER_HEX "${key.publicKeyDerHex}"`,
  'the firmware OTA public key'
);
const projectKeyExpression = /LUDANT_OTA_PUBLIC_KEY = "[^"]*";/g;
if (!originalProject.match(projectKeyExpression)?.length) {
  throw new Error('Could not find the iOS OTA public key build setting');
}
let updatedProject = originalProject.replace(
  projectKeyExpression,
  `LUDANT_OTA_PUBLIC_KEY = "${key.publicKeyRawBase64}";`
);
const packageNames = new Set([
  packageName,
  ...(fs.existsSync(assetsDirectory)
    ? fs.readdirSync(assetsDirectory, { withFileTypes: true })
      .filter((entry) => entry.isDirectory() && entry.name.endsWith('.ludantfirmware'))
      .map((entry) => entry.name)
    : [])
]);
updatedProject = removeFirmwareAssetReferences(updatedProject, packageNames);
updatedProject = addFirmwareAssetToXcodeProject(updatedProject, packageName);
const updatedCatalog = {
  ...catalog,
  versions: [...catalog.versions, {
    version: nextVersion,
    asset: `FirmwareAssets/${packageName}`
  }].sort((left, right) => compareVersions(left.version, right.version))
};

let packageCreated = false;
let committed = false;
const buildDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'ludant-arduino-release-'));
try {
  fs.writeFileSync(versionHeaderPath, updatedVersionHeader);
  fs.writeFileSync(authHeaderPath, updatedAuthHeader);

  const fqbn = process.env.LUDANT_ARDUINO_FQBN ?? 'esp32:esp32:esp32s3:CDCOnBoot=cdc';
  const build = spawnSync('arduino-cli', [
    'compile',
    '--fqbn', fqbn,
    '--build-path', buildDirectory,
    '--export-binaries',
    sketchDirectory
  ], { cwd: root, env: process.env, stdio: 'inherit' });
  if (build.error) throw new Error(`Could not run arduino-cli: ${build.error.message}`);
  if (build.status !== 0) throw new Error(`arduino-cli compile failed with exit code ${build.status}`);

  const binaryPath = findApplicationBinary(buildDirectory);
  // ESP image files place the 256-byte app descriptor at file offset 0x20;
  // its version field begins 16 bytes into that descriptor. Fail the release
  // before signing if the binary identity did not follow the release version.
  const embeddedVersion = fs.readFileSync(binaryPath)
    .subarray(0x30, 0x50)
    .toString('utf8')
    .split('\0', 1)[0];
  if (embeddedVersion !== nextVersion) {
    throw new Error(`Arduino image metadata version is ${embeddedVersion || '<empty>'}; expected ${nextVersion}`);
  }
  fs.mkdirSync(assetsDirectory, { recursive: true });
  const signing = spawnSync(process.execPath, [
    releaseToolPath,
    binaryPath,
    packagePath,
    '--version', nextVersion,
    '--minimum-partition-size', maximumPartitionSize
  ], {
    cwd: root,
    env: { ...process.env, LUDANT_OTA_SIGNING_PRIVATE_KEY: key.pem },
    stdio: 'inherit'
  });
  if (signing.error) throw new Error(`Could not run the artifact signer: ${signing.error.message}`);
  if (signing.status !== 0) throw new Error(`Artifact signing failed with exit code ${signing.status}`);
  packageCreated = true;

  fs.writeFileSync(projectPath, updatedProject);
  fs.writeFileSync(catalogPath, `${JSON.stringify(updatedCatalog, null, 2)}\n`);
  committed = true;

  console.log(`\nCreated ${path.relative(root, packagePath)}`);
  console.log(`Firmware public key DER: ${key.publicKeyDerHex}`);
  console.log(`iOS public key Base64:   ${key.publicKeyRawBase64}`);
  console.log('Build the iOS app, flash this firmware over USB once, then test the catalog update over BLE.');
} finally {
  fs.rmSync(buildDirectory, { recursive: true, force: true });
  if (!committed) {
    fs.writeFileSync(versionHeaderPath, originalVersionHeader);
    fs.writeFileSync(authHeaderPath, originalAuthHeader);
    fs.writeFileSync(projectPath, originalProject);
    fs.writeFileSync(catalogPath, originalCatalog);
    if (packageCreated) fs.rmSync(packagePath, { recursive: true, force: true });
  }
}
