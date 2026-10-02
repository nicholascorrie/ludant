import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createUsbFlashPackage, readUsbFlashPackageManifest, validateUsbFlashPackage } from './create-usb-flash-package.mjs';
import { createLaunchpadFlashPackage, launchpadShareURL } from './create-launchpad-flash-package.mjs';

function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ludant-usb-package-test-'));
  const build = path.join(root, 'build');
  const ota = path.join(root, '2.4.6.ludantfirmware');
  fs.mkdirSync(path.join(build, 'bootloader'), { recursive: true });
  fs.mkdirSync(path.join(build, 'partition_table'), { recursive: true });
  fs.mkdirSync(ota);

  const app = Buffer.alloc(128, 0xa5);
  Buffer.from('2.4.6\0').copy(app, 0x30);
  const flashFiles = {
    '0x0': 'bootloader/bootloader.bin',
    '0x10000': 'partition_table/partition-table.bin',
    '0x17000': 'ota_data_initial.bin',
    '0x120000': 'ludant_esp32s3_ota.bin'
  };
  const imageBytes = {
    'bootloader/bootloader.bin': Buffer.from('bootloader'),
    'partition_table/partition-table.bin': Buffer.from('partition-table'),
    'ota_data_initial.bin': Buffer.from('ota-data'),
    'ludant_esp32s3_ota.bin': app
  };
  for (const [relativePath, data] of Object.entries(imageBytes)) {
    fs.mkdirSync(path.dirname(path.join(build, relativePath)), { recursive: true });
    fs.writeFileSync(path.join(build, relativePath), data);
  }
  fs.writeFileSync(path.join(build, 'flasher_args.json'), JSON.stringify({
    flash_settings: { flash_mode: 'dio', flash_freq: '80m', flash_size: '4MB' },
    flash_files: flashFiles,
    bootloader: { offset: '0x0', file: flashFiles['0x0'] },
    'partition-table': { offset: '0x10000', file: flashFiles['0x10000'] },
    otadata: { offset: '0x17000', file: flashFiles['0x17000'] },
    app: { offset: '0x120000', file: flashFiles['0x120000'] },
    extra_esptool_args: { chip: 'esp32s3' }
  }, null, 2));
  fs.writeFileSync(path.join(ota, 'firmware.bin'), app);
  fs.writeFileSync(path.join(ota, 'manifest.json'), JSON.stringify({ firmwareVersion: '2.4.6', sha256: sha256(app) }));
  return { root, build, ota, app };
}

test('creates a version-matched USB ZIP with build-derived offsets, checksums, and scripts', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  const outputPath = path.join(sample.root, '2.4.6-usb.zip');

  createUsbFlashPackage({ buildDirectory: sample.build, version: '2.4.6', outputPath, otaPackagePath: sample.ota });
  const manifest = validateUsbFlashPackage(outputPath, '2.4.6');

  assert.equal(manifest.bootloader.offset, '0x0');
  assert.deepEqual(manifest.projectImages.map((entry) => entry.offset), ['0x10000', '0x17000', '0x120000']);
  assert.equal(manifest.projectImages.at(-1).sha256, sha256(sample.app));
  assert.ok(fs.existsSync(outputPath));
  assert.ok(readUsbFlashPackageManifest(outputPath).firmwareVersion === '2.4.6');
});

test('creates a merged Launchpad image and public config from the validated USB flash map', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  const usbPackagePath = path.join(sample.root, '2.4.6-usb.zip');
  const outputDirectory = path.join(sample.root, 'launchpad');
  createUsbFlashPackage({ buildDirectory: sample.build, version: '2.4.6', outputPath: usbPackagePath, otaPackagePath: sample.ota });

  const result = createLaunchpadFlashPackage({
    usbPackagePath,
    version: '2.4.6',
    outputDirectory,
    publicBaseURL: 'https://example.github.io/ludant/launchpad/'
  });
  const imagePath = path.join(outputDirectory, 'ludant-2.4.6.bin');
  const image = fs.readFileSync(imagePath);
  const config = fs.readFileSync(path.join(outputDirectory, 'config.toml'), 'utf8');
  const readme = fs.readFileSync(path.join(outputDirectory, 'README.md'), 'utf8');

  assert.equal(image.length, 4 * 1024 * 1024);
  assert.deepEqual(image.subarray(0, Buffer.byteLength('bootloader')), Buffer.from('bootloader'));
  assert.ok(image.subarray(0x10000, 0x10000 + Buffer.byteLength('partition-table')).equals(Buffer.from('partition-table')));
  assert.ok(image.subarray(0x120000, 0x120000 + sample.app.length).equals(sample.app));
  assert.equal(image[0x8000], 0xff);
  assert.match(config, /firmware_images_url = "https:\/\/example\.github\.io\/ludant\/launchpad\/"/);
  assert.match(config, /image\.esp32-s3 = "ludant-2\.4\.6\.bin"/);
  assert.equal(result.manifest.firmwareVersion, '2.4.6');
  assert.equal(result.manifest.imageSha256, sha256(image));
  assert.match(readme, /does not burn eFuses/);
  assert.match(readme, /install other compatible firmware later/);
  assert.equal(new URL(result.shareURL).searchParams.get('flashConfigURL'), result.configURL);
  assert.equal(new URL(launchpadShareURL(result.configURL)).searchParams.has('crossDomain'), false);
});

test('rejects a Launchpad package whose version differs from its USB source package', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  const usbPackagePath = path.join(sample.root, '2.4.6-usb.zip');
  createUsbFlashPackage({ buildDirectory: sample.build, version: '2.4.6', outputPath: usbPackagePath, otaPackagePath: sample.ota });

  assert.throws(() => createLaunchpadFlashPackage({
    usbPackagePath,
    version: '2.4.7',
    outputDirectory: path.join(sample.root, 'launchpad')
  }), /version mismatch/);
});

test('rejects a USB build whose application differs from the signed OTA package', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  fs.writeFileSync(path.join(sample.build, 'ludant_esp32s3_ota.bin'), Buffer.from('different image'));

  assert.throws(() => createUsbFlashPackage({
    buildDirectory: sample.build,
    version: '2.4.6',
    outputPath: path.join(sample.root, '2.4.6-usb.zip'),
    otaPackagePath: sample.ota
  }), /application metadata|does not match/);
});

test('rejects unsafe paths in the ESP-IDF generated flash map', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  const argsPath = path.join(sample.build, 'flasher_args.json');
  const args = JSON.parse(fs.readFileSync(argsPath, 'utf8'));
  args.app.file = '../outside.bin';
  fs.writeFileSync(argsPath, JSON.stringify(args));

  assert.throws(() => createUsbFlashPackage({
    buildDirectory: sample.build,
    version: '2.4.6',
    outputPath: path.join(sample.root, '2.4.6-usb.zip'),
    otaPackagePath: sample.ota
  }), /unsafe ESP-IDF flash image/);
});

test('rejects corrupted files inside a generated USB ZIP', (t) => {
  const sample = fixture();
  t.after(() => fs.rmSync(sample.root, { recursive: true, force: true }));
  const outputPath = path.join(sample.root, '2.4.6-usb.zip');
  createUsbFlashPackage({ buildDirectory: sample.build, version: '2.4.6', outputPath, otaPackagePath: sample.ota });
  const rewrite = path.join(sample.root, 'rewrite');
  fs.mkdirSync(rewrite);
  const unzip = spawnSync('unzip', ['-q', outputPath, '-d', rewrite]);
  assert.equal(unzip.status, 0);
  fs.writeFileSync(path.join(rewrite, 'images/bootloader.bin'), Buffer.from('tampered'));
  const archive = path.join(sample.root, 'corrupted.zip');
  const result = spawnSync('zip', ['-X', '-r', archive, 'manifest.json', 'README.txt', 'flash.py', 'flash-macos.command', 'flash-windows.bat', 'images'], { cwd: rewrite });
  assert.equal(result.status, 0);
  assert.throws(() => validateUsbFlashPackage(archive, '2.4.6'), /checksum mismatch/);
});
