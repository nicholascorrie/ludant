#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { readUsbFlashPackageManifest, validateUsbFlashPackage } from './create-usb-flash-package.mjs';

export const LAUNCHPAD_BASE_URL = 'https://nicholascorrie.github.io/ludant/launchpad/';
export const ESP_LAUNCHPAD_URL = 'https://espressif.github.io/esp-launchpad/';

function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

function readImage(zipPath, imagePath) {
  const result = spawnSync('unzip', ['-p', path.resolve(zipPath), imagePath], { encoding: null });
  if (result.error) throw new Error(`Could not read ${imagePath} from USB package: ${result.error.message}`);
  if (result.status !== 0) throw new Error(`Could not read ${imagePath} from USB package`);
  return result.stdout;
}

function parseFlashSize(value) {
  const match = String(value).match(/^(\d+)(KB|MB)$/i);
  if (!match) throw new Error(`Unsupported ESP32 flash size: ${value}`);
  return Number(match[1]) * (match[2].toUpperCase() === 'MB' ? 1024 * 1024 : 1024);
}

function appVersion(data) {
  return data.subarray(0x30, 0x50).toString('utf8').split('\0', 1)[0];
}

function tomlString(value) {
  return JSON.stringify(value);
}

export function launchpadConfigURL(baseURL = LAUNCHPAD_BASE_URL) {
  return new URL('config.toml', baseURL.endsWith('/') ? baseURL : `${baseURL}/`).toString();
}

export function launchpadShareURL(configURL) {
  const url = new URL(ESP_LAUNCHPAD_URL);
  url.searchParams.set('flashConfigURL', configURL);
  return url.toString();
}

export function createLaunchpadFlashPackage({ usbPackagePath, version, outputDirectory, publicBaseURL = LAUNCHPAD_BASE_URL }) {
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error(`Invalid firmware version: ${version}`);
  const manifest = validateUsbFlashPackage(usbPackagePath, version);
  if (manifest.chip !== 'esp32s3') throw new Error(`Launchpad package expected esp32s3, got ${manifest.chip}`);
  const flashSizeBytes = parseFlashSize(manifest.flashSettings.flash_size);
  const images = [manifest.bootloader, ...manifest.projectImages]
    .map((entry) => {
      const offset = Number.parseInt(entry.offset, 16);
      if (!Number.isSafeInteger(offset) || offset < 0) throw new Error(`Invalid flash offset: ${entry.offset}`);
      const data = readImage(usbPackagePath, entry.path);
      if (data.length !== entry.size || sha256(data) !== entry.sha256) throw new Error(`USB package image checksum mismatch: ${entry.path}`);
      if (offset + data.length > flashSizeBytes) throw new Error(`Flash image exceeds ${manifest.flashSettings.flash_size}: ${entry.path}`);
      return { ...entry, offset, data };
    })
    .sort((left, right) => left.offset - right.offset);

  for (let index = 1; index < images.length; index += 1) {
    if (images[index - 1].offset + images[index - 1].data.length > images[index].offset) {
      throw new Error(`ESP-IDF flash images overlap at ${images[index].offset.toString(16)}`);
    }
  }
  const appImage = images.find((entry) => entry.path.endsWith('/ludant_esp32s3_ota.bin'));
  if (!appImage || appVersion(appImage.data) !== version) {
    throw new Error(`USB application image does not match Launchpad release ${version}`);
  }

  const baseURL = publicBaseURL.endsWith('/') ? publicBaseURL : `${publicBaseURL}/`;
  const imageName = `ludant-${version}.bin`;
  const outputPath = path.resolve(outputDirectory);
  fs.mkdirSync(outputPath, { recursive: true });
  const mergedImage = Buffer.alloc(flashSizeBytes, 0xff);
  for (const image of images) image.data.copy(mergedImage, image.offset);
  fs.writeFileSync(path.join(outputPath, imageName), mergedImage);

  const config = [
    'esp_toml_version = 1.0',
    `firmware_images_url = ${tomlString(baseURL)}`,
    'supported_apps = ["ludant"]',
    '',
    '[ludant]',
    'chipsets = ["ESP32-S3"]',
    `image.esp32-s3 = ${tomlString(imageName)}`,
    `description = ${tomlString(`Ludant controller firmware ${version}`)}`,
    `readme.text = ${tomlString(`${baseURL}README.md`)}`,
    ''
  ].join('\n');
  fs.writeFileSync(path.join(outputPath, 'config.toml'), config);

  const launchpadManifest = {
    formatVersion: 1,
    product: manifest.product,
    hardware: manifest.hardware,
    firmwareVersion: version,
    chip: manifest.chip,
    image: imageName,
    imageSize: mergedImage.length,
    imageSha256: sha256(mergedImage),
    flashSettings: manifest.flashSettings,
    sourceImages: images.map(({ offset, path: imagePath, size, sha256: digest }) => ({
      offset: `0x${offset.toString(16)}`,
      path: imagePath,
      size,
      sha256: digest
    }))
  };
  fs.writeFileSync(path.join(outputPath, 'manifest.json'), `${JSON.stringify(launchpadManifest, null, 2)}\n`);
  fs.writeFileSync(path.join(outputPath, 'README.md'), [
    `# Ludant ESP32-S3 firmware ${version}`,
    '',
    'Use this on a supported ESP32-S3 board with at least 4 MB of flash. Connect it to a Mac or Windows computer with a USB data cable, then open the Ludant firmware link in ESP Launchpad using Chrome or Edge.',
    '',
    'Flashing replaces the board’s current firmware and data. It does not burn eFuses, enable Secure Boot, or enable flash encryption. Your board remains open so you can install other compatible firmware later.',
    '',
    'ESP Launchpad does not run the USB ZIP installer’s eFuse checks. If the board may already use Secure Boot or flash encryption, use the USB ZIP installer instead; it checks first and refuses before writing.',
    '',
    'After flashing, return to Ludant on your iPhone and connect to the controller over Bluetooth.',
    ''
  ].join('\n'));

  return {
    outputDirectory: outputPath,
    configURL: launchpadConfigURL(baseURL),
    shareURL: launchpadShareURL(launchpadConfigURL(baseURL)),
    manifest: launchpadManifest
  };
}
