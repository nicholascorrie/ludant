#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { validateUsbFlashPackage } from './create-usb-flash-package.mjs';
import { LAUNCHPAD_BASE_URL, launchpadConfigURL } from './create-launchpad-flash-package.mjs';

const root = path.resolve(import.meta.dirname, '../../../..');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const assetsRoot = path.join(root, 'apps/ui/ludant-ios/ludant-ios');
const launchpadRoot = path.join(root, 'docs/launchpad');
const projectPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios.xcodeproj/project.pbxproj');
const otaSlotSize = 1_310_720;
const versionPattern = /(?:^|-)(\d+)\.(\d+)\.(\d+)$/;
const xcodeProject = fs.readFileSync(projectPath, 'utf8');

function fail(message) {
  throw new Error(message);
}

function versionParts(value) {
  const match = value.match(versionPattern);
  return match ? match.slice(1).map(Number) : null;
}

function compareVersions(left, right) {
  const a = versionParts(left);
  const b = versionParts(right);
  if (!a || !b) return 0;
  return a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
}

function assertPackageIsBundled(packageName) {
  const lines = xcodeProject.split('\n');
  const fileReferenceLine = lines.find((line) =>
    line.includes('= {isa = PBXFileReference;') &&
    line.includes(`path = "ludant-ios/FirmwareAssets/${packageName}"`)
  );
  const fileReferenceId = fileReferenceLine?.match(/^\s*([A-F0-9]{24})/)?.[1];
  if (!fileReferenceId) fail(`Xcode project has no folder reference for ${packageName}`);

  const buildFileLine = lines.find((line) =>
    line.includes('= {isa = PBXBuildFile;') &&
    line.includes(`fileRef = ${fileReferenceId} /*`)
  );
  const buildFileId = buildFileLine?.match(/^\s*([A-F0-9]{24})/)?.[1];
  if (!buildFileId) fail(`Xcode project does not copy ${packageName} as an app resource`);

  const resourcesStart = xcodeProject.indexOf('/* Begin PBXResourcesBuildPhase section */');
  const resourcesEnd = xcodeProject.indexOf('/* End PBXResourcesBuildPhase section */', resourcesStart);
  const resources = resourcesStart >= 0 && resourcesEnd > resourcesStart
    ? xcodeProject.slice(resourcesStart, resourcesEnd)
    : '';
  if (!resources.includes(buildFileId)) {
    fail(`Xcode app Resources phase omits ${packageName}`);
  }
}

function publicKey() {
  const configured = process.env.LUDANT_OTA_PUBLIC_KEY_BASE64;
  const project = fs.readFileSync(projectPath, 'utf8');
  const match = project.match(/LUDANT_OTA_PUBLIC_KEY = "([^"]+)";/);
  const value = configured ?? match?.[1];
  if (!value) fail('No OTA public key configured');
  const raw = Buffer.from(value, 'base64');
  if (raw.length !== 32) fail('OTA public key must contain 32 raw Ed25519 bytes');
  return crypto.createPublicKey({ key: Buffer.concat([
    Buffer.from('302a300506032b6570032100', 'hex'),
    raw
  ]), format: 'der', type: 'spki' });
}

const catalog = JSON.parse(fs.readFileSync(catalogPath, 'utf8'));
if (catalog.catalogVersion !== 1 || catalog.product !== 'ludant-esp32s3' || catalog.hardware !== 'ESP32-S3') {
  fail('catalog header is invalid');
}
if (!Array.isArray(catalog.versions) || catalog.versions.length === 0) fail('catalog must contain releases');
if (new Set(catalog.versions.map((entry) => entry.version)).size !== catalog.versions.length) fail('catalog contains duplicate versions');

const key = publicKey();
let previousSecureVersion = -1;
const releases = [...catalog.versions].sort((a, b) => compareVersions(a.version, b.version));
for (const release of releases) {
  if (!release || typeof release.version !== 'string' || !versionParts(release.version)) fail(`invalid release version: ${release?.version}`);
  if (typeof release.asset !== 'string' || release.asset.startsWith('/') || release.asset.includes('..')) fail(`unsafe asset reference: ${release.asset}`);
  const packagePath = path.resolve(assetsRoot, release.asset);
  if (!packagePath.startsWith(`${assetsRoot}${path.sep}`)) fail(`asset escapes resource root: ${release.asset}`);
  if (!fs.statSync(packagePath, { throwIfNoEntry: false })?.isDirectory()) fail(`missing package: ${release.asset}`);
  assertPackageIsBundled(path.basename(packagePath));
  const manifestPath = path.join(packagePath, 'manifest.json');
  const binaryPath = path.join(packagePath, 'firmware.bin');
  if (!fs.existsSync(manifestPath) || !fs.existsSync(binaryPath)) fail(`incomplete package: ${release.asset}`);
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const binary = fs.readFileSync(binaryPath);
  if (manifest.firmwareVersion !== release.version) fail(`catalog/manifest version mismatch: ${release.version}`);
  if (manifest.product !== catalog.product || manifest.hardware !== catalog.hardware) fail(`package target mismatch: ${release.version}`);
  if (!Number.isSafeInteger(manifest.size) || manifest.size !== binary.length || binary.length === 0 || binary.length > otaSlotSize) fail(`invalid package size: ${release.version}`);
  if (manifest.minimumPartitionSize !== undefined &&
      (!Number.isSafeInteger(manifest.minimumPartitionSize) || manifest.minimumPartitionSize < binary.length || manifest.minimumPartitionSize > otaSlotSize)) {
    fail(`invalid minimum partition size: ${release.version}`);
  }
  const digest = crypto.createHash('sha256').update(binary).digest();
  if (manifest.sha256 !== digest.toString('hex')) fail(`hash mismatch: ${release.version}`);
  if (!manifest.signature || manifest.signature.algorithm?.toLowerCase() !== 'ed25519') fail(`unsupported signature algorithm: ${release.version}`);
  const signature = Buffer.from(manifest.signature.value ?? '', 'base64');
  if (signature.length !== 64 || !crypto.verify(null, digest, key, signature)) fail(`signature mismatch: ${release.version}`);
  if (!['esp-idf', 'arduino'].includes(manifest.runtime ?? 'arduino')) fail(`unsupported runtime: ${release.version}`);
  const secureVersion = manifest.secureVersion === undefined ? 0 : manifest.secureVersion;
  if (!Number.isSafeInteger(secureVersion) || secureVersion < 0) fail(`invalid secure version: ${release.version}`);
  if (manifest.secureVersion !== undefined) {
    if (secureVersion < previousSecureVersion) fail(`secure version regressed: ${release.version}`);
    previousSecureVersion = secureVersion;
  }
  if (manifest.runtime === 'esp-idf' &&
      (!manifest.moduleImplementations || typeof manifest.moduleImplementations !== 'object' ||
       !manifest.moduleSchemas || typeof manifest.moduleSchemas !== 'object')) {
    fail(`ESP-IDF package is missing module metadata: ${release.version}`);
  }
  if (release.usbAsset !== undefined) {
    if (typeof release.usbAsset !== 'string' || release.usbAsset.startsWith('/') || release.usbAsset.includes('..')) {
      fail(`unsafe USB flash package reference: ${release.usbAsset}`);
    }
    const usbPackagePath = path.resolve(assetsRoot, release.usbAsset);
    if (!usbPackagePath.startsWith(`${path.join(assetsRoot, 'FirmwareAssets')}${path.sep}`)) {
      fail(`USB flash package escapes FirmwareAssets: ${release.usbAsset}`);
    }
    if (!fs.statSync(usbPackagePath, { throwIfNoEntry: false })?.isFile()) fail(`missing USB flash package: ${release.usbAsset}`);
    assertPackageIsBundled(path.basename(usbPackagePath));
    const usbManifest = validateUsbFlashPackage(usbPackagePath, release.version);
    const appImage = usbManifest.projectImages?.find((image) => image.path.endsWith('/ludant_esp32s3_ota.bin'));
    if (!appImage || appImage.sha256 !== manifest.sha256) fail(`USB/OTA application image mismatch: ${release.version}`);
  }
  if (manifest.runtime !== 'esp-idf') console.warn(`warning: legacy runtime package ${release.version}`);
}

if (process.env.LUDANT_REQUIRE_ESPIDF === '1') {
  const legacy = releases.filter((release) => {
    const manifest = JSON.parse(fs.readFileSync(path.join(assetsRoot, release.asset, 'manifest.json'), 'utf8'));
    return manifest.runtime !== 'esp-idf';
  });
  if (legacy.length > 0) fail(`production catalog contains legacy runtime packages: ${legacy.map((release) => release.version).join(', ')}`);
}

if (!catalog.latest || typeof catalog.latest.version !== 'string' || typeof catalog.latest.asset !== 'string') {
  fail('catalog latest pointer must include version and asset');
}
const latestRelease = releases.find((release) => release.version === catalog.latest.version);
if (!latestRelease) fail(`catalog latest version does not resolve to an artifact: ${catalog.latest.version}`);
if (latestRelease.asset !== catalog.latest.asset) fail(`catalog latest asset does not match release ${catalog.latest.version}`);
if (catalog.latest.usbAsset !== latestRelease.usbAsset) fail(`catalog latest USB package does not match release ${catalog.latest.version}`);
if (catalog.latest.launchpadConfigURL !== latestRelease.launchpadConfigURL) fail(`catalog latest Launchpad config does not match release ${catalog.latest.version}`);
if (process.env.LUDANT_REQUIRE_ESPIDF === '1' && typeof latestRelease.usbAsset !== 'string') {
  fail(`production catalog has no USB first-flash package for latest release ${catalog.latest.version}`);
}
if (releases.some((release) => release.version !== latestRelease.version && release.usbAsset !== undefined)) {
  fail('USB first-flash packages should be embedded only for the latest release');
}

if (process.env.LUDANT_REQUIRE_ESPIDF === '1' && typeof latestRelease.launchpadConfigURL !== 'string') {
  fail(`production catalog has no Launchpad config URL for latest release ${catalog.latest.version}`);
}
if (latestRelease.launchpadConfigURL) {
  if (latestRelease.launchpadConfigURL !== launchpadConfigURL(LAUNCHPAD_BASE_URL)) {
    fail(`catalog Launchpad config URL is not the configured Pages URL: ${latestRelease.launchpadConfigURL}`);
  }
  if (releases.some((release) => release.version !== latestRelease.version && release.launchpadConfigURL !== undefined)) {
    fail('Launchpad config URLs should be recorded only for the latest release');
  }
  const configPath = path.join(launchpadRoot, 'config.toml');
  const launchpadManifestPath = path.join(launchpadRoot, 'manifest.json');
  const config = fs.readFileSync(configPath, 'utf8');
  const launchpadManifest = JSON.parse(fs.readFileSync(launchpadManifestPath, 'utf8'));
  if (launchpadManifest.formatVersion !== 1 || launchpadManifest.product !== catalog.product || launchpadManifest.hardware !== catalog.hardware || launchpadManifest.chip !== 'esp32s3') {
    fail('Launchpad manifest product or format is invalid');
  }
  if (launchpadManifest.firmwareVersion !== latestRelease.version) fail('Launchpad/catalog version mismatch');
  if (typeof launchpadManifest.image !== 'string' || path.basename(launchpadManifest.image) !== launchpadManifest.image) fail('Launchpad image path is unsafe');
  const launchpadImagePath = path.join(launchpadRoot, launchpadManifest.image);
  const launchpadImage = fs.readFileSync(launchpadImagePath);
  if (launchpadImage.length !== launchpadManifest.imageSize || crypto.createHash('sha256').update(launchpadImage).digest('hex') !== launchpadManifest.imageSha256) {
    fail('Launchpad merged image size or checksum mismatch');
  }
  if (!config.includes(`image.esp32-s3 = "${launchpadManifest.image}"`) || !config.includes(`firmware_images_url = "${LAUNCHPAD_BASE_URL}"`)) {
    fail('Launchpad config does not reference its image directory and generated binary');
  }
  if (!fs.existsSync(path.join(launchpadRoot, 'README.md'))) fail('Launchpad setup instructions are missing');
  const usbManifest = validateUsbFlashPackage(path.resolve(assetsRoot, latestRelease.usbAsset), latestRelease.version);
  const usbAppImage = usbManifest.projectImages.find((image) => image.path.endsWith('/ludant_esp32s3_ota.bin'));
  const launchpadAppImage = launchpadManifest.sourceImages.find((image) => image.path.endsWith('/ludant_esp32s3_ota.bin'));
  if (!usbAppImage || !launchpadAppImage || launchpadAppImage.sha256 !== usbAppImage.sha256) {
    fail('Launchpad application image does not match the USB/OTA release');
  }
}

console.log(`Validated ${releases.length} firmware package(s); latest=${catalog.latest.version}${latestRelease.usbAsset ? ', USB package bundled' : ''}${latestRelease.launchpadConfigURL ? ', Launchpad assets aligned' : ''}`);
