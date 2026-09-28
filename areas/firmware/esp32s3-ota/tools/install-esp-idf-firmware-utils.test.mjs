import test from 'node:test';
import assert from 'node:assert/strict';
import { inspectBootLog, parseFuseSummary, lockedFlashReason, selectCatalogRelease } from './install-esp-idf-firmware-utils.mjs';

const virginFuseSummary = `
SECURE_BOOT_EN (BLOCK0) Set this bit to enable secure boot = False R/W (0b0)
SPI_BOOT_CRYPT_CNT (BLOCK0) Enables flash encryption = Disable R/W (0b000)
`;
const fuseFields = ['SECURE_BOOT_EN', 'SPI_BOOT_CRYPT_CNT'];

test('allows ordinary USB flashing when Secure Boot and flash encryption are off', () => {
  const parsed = parseFuseSummary(virginFuseSummary, fuseFields);
  assert.deepEqual(parsed.missing, []);
  assert.equal(lockedFlashReason(parsed.values), null);
});

test('fails closed when an eFuse field is missing', () => {
  const parsed = parseFuseSummary(virginFuseSummary.replace(/SPI_BOOT_CRYPT_CNT.*\n/, ''), fuseFields);
  assert.deepEqual(parsed.missing, ['SPI_BOOT_CRYPT_CNT']);
});

test('refuses to overwrite a device that is already permanently secured', () => {
  for (const [field, activeValue] of [
    ['SECURE_BOOT_EN', '= True R/- (0b1)'],
    ['SPI_BOOT_CRYPT_CNT', '= Enable R/- (0b001)']
  ]) {
    const activeSummary = virginFuseSummary.replace(new RegExp(`^${field}.*$`, 'm'), `${field} (BLOCK0) state ${activeValue}`);
    const parsed = parseFuseSummary(activeSummary, fuseFields);
    assert.match(lockedFlashReason(parsed.values), new RegExp(`${field} is already active`));
  }
});

test('post-flash monitor differentiates advertising, panic, and incomplete boot', () => {
  assert.equal(inspectBootLog('I (100) BLE advertising as Ludant\nI (101) Firmware ready; version=2.0.29', '2.0.29'), 'ready');
  assert.equal(inspectBootLog('I (100) BLE advertising as Ludant\nI (101) Firmware ready; version=2.0.28', '2.0.29'), 'version-mismatch');
  assert.equal(inspectBootLog('Guru Meditation Error: Core 0 panic', '2.0.29'), 'crashed');
  assert.equal(inspectBootLog('Firmware starting', '2.0.29'), 'pending');
});

test('default release selection follows the catalog version and asset pointer', () => {
  const catalog = {
    latest: { version: '2.0.30', asset: 'FirmwareAssets/2.0.30.ludantfirmware' },
    versions: [
      { version: '2.0.29', asset: 'FirmwareAssets/2.0.29.ludantfirmware' },
      { version: '2.0.30', asset: 'FirmwareAssets/2.0.30.ludantfirmware' }
    ]
  };
  assert.equal(selectCatalogRelease(catalog).version, '2.0.30');
  assert.equal(selectCatalogRelease(catalog, '2.0.29').version, '2.0.29');
  assert.throws(() => selectCatalogRelease({ ...catalog, latest: { version: '2.0.30', asset: 'wrong-path' } }), /latest pointer does not match/);
});
