#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '../../../..');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const assetsRoot = path.join(root, 'apps/ui/ludant-ios/ludant-ios');
const projectPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios.xcodeproj/project.pbxproj');
const otaSlotSize = 1_310_720;
const versionPattern = /(?:^|-)(\d+)\.(\d+)\.(\d+)$/;

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
  if (manifest.runtime === 'esp-idf' &&
      (!manifest.moduleImplementations || typeof manifest.moduleImplementations !== 'object' ||
       !manifest.moduleSchemas || typeof manifest.moduleSchemas !== 'object')) {
    fail(`ESP-IDF package is missing module metadata: ${release.version}`);
  }
  if (secureVersion < previousSecureVersion) fail(`secure version regressed: ${release.version}`);
  previousSecureVersion = secureVersion;
  if (manifest.runtime !== 'esp-idf') console.warn(`warning: legacy runtime package ${release.version}`);
}

if (process.env.LUDANT_REQUIRE_ESPIDF === '1') {
  const legacy = releases.filter((release) => {
    const manifest = JSON.parse(fs.readFileSync(path.join(assetsRoot, release.asset, 'manifest.json'), 'utf8'));
    return manifest.runtime !== 'esp-idf';
  });
  if (legacy.length > 0) fail(`production catalog contains legacy runtime packages: ${legacy.map((release) => release.version).join(', ')}`);
}

const latestVersion = catalog.latest?.version ?? releases.at(-1).version;
if (!releases.some((release) => release.version === latestVersion)) fail(`catalog latest version does not resolve to an artifact: ${latestVersion}`);

console.log(`Validated ${releases.length} firmware package(s); latest=${latestVersion}`);
