import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { beginUsbFlashAssetTransaction } from './usb-flash-asset-transaction.mjs';

function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ludant-usb-assets-test-'));
  const assets = path.join(root, 'FirmwareAssets');
  fs.mkdirSync(assets);
  fs.writeFileSync(path.join(assets, '2.0.44-usb.zip'), 'previous');
  return { root, assets };
}

test('restores the previous ZIP when a release fails after creating its new ZIP', () => {
  const { root, assets } = fixture();
  try {
    const transaction = beginUsbFlashAssetTransaction(assets);
    assert.deepEqual(fs.readdirSync(assets), []);
    fs.writeFileSync(path.join(assets, '2.0.45-usb.zip'), 'partial release');
    transaction.rollback();
    assert.deepEqual(fs.readdirSync(assets), ['2.0.44-usb.zip']);
    assert.equal(fs.readFileSync(path.join(assets, '2.0.44-usb.zip'), 'utf8'), 'previous');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('commits only the new latest USB ZIP and removes the backup', () => {
  const { root, assets } = fixture();
  try {
    const transaction = beginUsbFlashAssetTransaction(assets);
    fs.writeFileSync(path.join(assets, '2.0.45-usb.zip'), 'new release');
    transaction.commit();
    assert.deepEqual(fs.readdirSync(assets), ['2.0.45-usb.zip']);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('restores the previous Pages package when a release fails', () => {
  const { root, assets } = fixture();
  const launchpad = path.join(root, 'docs', 'launchpad');
  fs.mkdirSync(launchpad, { recursive: true });
  fs.writeFileSync(path.join(launchpad, 'config.toml'), 'previous config');
  try {
    const transaction = beginUsbFlashAssetTransaction(assets, launchpad);
    assert.equal(fs.existsSync(launchpad), false);
    fs.mkdirSync(launchpad, { recursive: true });
    fs.writeFileSync(path.join(launchpad, 'config.toml'), 'partial release');
    transaction.rollback();
    assert.equal(fs.readFileSync(path.join(launchpad, 'config.toml'), 'utf8'), 'previous config');
    assert.deepEqual(fs.readdirSync(assets), ['2.0.44-usb.zip']);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('commits the new Pages package with the latest USB ZIP', () => {
  const { root, assets } = fixture();
  const launchpad = path.join(root, 'docs', 'launchpad');
  fs.mkdirSync(launchpad, { recursive: true });
  fs.writeFileSync(path.join(launchpad, 'config.toml'), 'previous config');
  try {
    const transaction = beginUsbFlashAssetTransaction(assets, launchpad);
    fs.writeFileSync(path.join(assets, '2.0.45-usb.zip'), 'new release');
    fs.mkdirSync(launchpad, { recursive: true });
    fs.writeFileSync(path.join(launchpad, 'config.toml'), 'new config');
    transaction.commit();
    assert.equal(fs.readFileSync(path.join(launchpad, 'config.toml'), 'utf8'), 'new config');
    assert.deepEqual(fs.readdirSync(assets), ['2.0.45-usb.zip']);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
