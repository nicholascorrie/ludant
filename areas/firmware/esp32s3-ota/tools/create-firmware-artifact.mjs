#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const OTA_SLOT_SIZE = 1_310_720;

function usage() {
  console.error('Usage: create-firmware-artifact.mjs <firmware.bin> <output.ludantfirmware> --version <version> [--capabilities a,b] [--minimum-boot-version 1.0.0] [--minimum-partition-size bytes] [--release-notes text] [--key-id id]');
  process.exit(2);
}

const [binaryPath, outputPath, ...args] = process.argv.slice(2);
if (!binaryPath || !outputPath) usage();
const options = {};
for (let index = 0; index < args.length; index += 2) {
  const key = args[index];
  const value = args[index + 1];
  if (!key?.startsWith('--') || value == null) usage();
  options[key.slice(2)] = value;
}

const privateKeyPem = process.env.LUDANT_OTA_SIGNING_PRIVATE_KEY;
if (!privateKeyPem) throw new Error('LUDANT_OTA_SIGNING_PRIVATE_KEY must contain the Ed25519 private key PEM');
if (!options.version) throw new Error('--version is required');

const firmware = fs.readFileSync(binaryPath);
if (firmware.length === 0) throw new Error('firmware image is empty');
if (firmware.length > OTA_SLOT_SIZE) {
  throw new Error(`firmware image is ${firmware.length} bytes; maximum OTA slot size is ${OTA_SLOT_SIZE}`);
}
const digest = crypto.createHash('sha256').update(firmware).digest();
const privateKey = crypto.createPrivateKey(privateKeyPem);
if (privateKey.asymmetricKeyType !== 'ed25519') throw new Error('signing key must be Ed25519');
const semverPattern = /(?:^|-)\d+\.\d+\.\d+$/;
if (!semverPattern.test(options.version)) {
  throw new Error('--version must end in semantic major.minor.patch form');
}
const minimumBootVersion = options['minimum-boot-version'] ?? '1.0.0';
if (!semverPattern.test(minimumBootVersion)) {
  throw new Error('--minimum-boot-version must end in semantic major.minor.patch form');
}
const signature = crypto.sign(null, digest, privateKey);
const publicKeyDer = crypto.createPublicKey(privateKey).export({ format: 'der', type: 'spki' });
if (publicKeyDer.length < 32) throw new Error('unexpected Ed25519 public-key encoding');
const publicKeyRaw = publicKeyDer.subarray(publicKeyDer.length - 32);
const minimumPartitionSize = options['minimum-partition-size'] ? Number(options['minimum-partition-size']) : undefined;
if (minimumPartitionSize !== undefined && (!Number.isSafeInteger(minimumPartitionSize) || minimumPartitionSize <= 0)) {
  throw new Error('--minimum-partition-size must be a positive integer');
}

const manifest = {
  product: 'ludant-esp32s3',
  hardware: 'ESP32-S3',
  firmwareVersion: options.version,
  otaProtocolVersion: 2,
  size: firmware.length,
  sha256: digest.toString('hex'),
  capabilities: (options.capabilities ?? 'modules,telemetry,ota').split(',').filter(Boolean),
  minimumBootVersion,
  ...(minimumPartitionSize !== undefined ? { minimumPartitionSize } : {}),
  ...(options['release-notes'] ? { releaseNotes: options['release-notes'] } : {}),
  signature: {
    algorithm: 'ed25519',
    value: signature.toString('base64'),
    ...(options['key-id'] ? { keyId: options['key-id'] } : {})
  }
};

const outputDirectory = path.resolve(outputPath);
const outputParent = path.dirname(outputDirectory);
fs.mkdirSync(outputParent, { recursive: true });
const temporaryDirectory = fs.mkdtempSync(path.join(outputParent, `.${path.basename(outputDirectory)}.tmp-`));
try {
  fs.copyFileSync(binaryPath, path.join(temporaryDirectory, 'firmware.bin'));
  fs.writeFileSync(path.join(temporaryDirectory, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  fs.rmSync(outputDirectory, { recursive: true, force: true });
  fs.renameSync(temporaryDirectory, outputDirectory);
} catch (error) {
  fs.rmSync(temporaryDirectory, { recursive: true, force: true });
  throw error;
}
console.log(JSON.stringify({
  package: outputDirectory,
  publicKeyRawBase64: publicKeyRaw.toString('base64'),
  publicKeyDerHex: publicKeyDer.toString('hex'),
  manifest
}, null, 2));
