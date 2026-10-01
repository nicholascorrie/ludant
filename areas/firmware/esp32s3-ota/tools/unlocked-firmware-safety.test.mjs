import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {
  assertUnlockedConfiguration,
  disableIrreversibleSecurityOptions,
  parseUsbFlashEfuses,
  usbFlashEfuseProblem
} from './unlocked-firmware-safety.mjs';

const openFuseSummary = `
SECURE_BOOT_EN (BLOCK0) Secure boot = False R/W (0b0)
SPI_BOOT_CRYPT_CNT (BLOCK0) Flash encryption = Disable R/W (0b000)
DIS_USB_SERIAL_JTAG (BLOCK0) Disable USB device = False R/W (0b0)
DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE (BLOCK0) Disable USB download = False R/W (0b0)
DIS_USB_OTG_DOWNLOAD_MODE (BLOCK0) Disable USB OTG download = False R/W (0b0)
`;

test('sanitizes stale ignored sdkconfig while retaining unrelated settings', () => {
  const stale = [
    'CONFIG_IDF_TARGET="esp32s3"',
    'CONFIG_FREERTOS_HZ=1000',
    'CONFIG_SECURE_FLASH_ENC_ENABLED=y',
    'CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y',
    'CONFIG_FLASH_ENCRYPTION_INSECURE=y',
    'CONFIG_FLASH_ENCRYPTION_UART_BOOTLOADER_ALLOW_ENCRYPT=y',
    'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y',
    'CONFIG_NVS_ENCRYPTION=y'
  ].join('\n');
  const safe = disableIrreversibleSecurityOptions(stale);
  assert.match(safe, /CONFIG_FREERTOS_HZ=1000/);
  assertUnlockedConfiguration(safe, 'test sdkconfig');
  assert.doesNotMatch(safe, /CONFIG_SECURE_FLASH_ENC_ENABLED=y/);
  assert.doesNotMatch(safe, /CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y/);
  assert.doesNotMatch(safe, /CONFIG_FLASH_ENCRYPTION_INSECURE=y/);
});

test('rejects a generated sdkconfig header that enables irreversible features', () => {
  assert.throws(
    () => assertUnlockedConfiguration('#define CONFIG_SECURE_BOOT_V2_ENABLED 1', 'test header'),
    /CONFIG_SECURE_BOOT_V2_ENABLED/
  );
  assert.throws(
    () => assertUnlockedConfiguration('#define CONFIG_FLASH_ENCRYPTION_ENABLED 1', 'test header'),
    /CONFIG_FLASH_ENCRYPTION_ENABLED/
  );
});

test('allows USB flashing only when eFuses show open security and a USB download route', () => {
  const parsed = parseUsbFlashEfuses(openFuseSummary);
  assert.deepEqual(parsed.missing, []);
  assert.equal(usbFlashEfuseProblem(parsed.values, parsed.missing), null);
});

test('fails closed on missing eFuse fields', () => {
  const incompleteSummary = openFuseSummary.replace(/^DIS_USB_OTG_DOWNLOAD_MODE.*\n/m, '');
  const parsed = parseUsbFlashEfuses(incompleteSummary);
  assert.deepEqual(parsed.missing, ['DIS_USB_OTG_DOWNLOAD_MODE']);
  assert.match(usbFlashEfuseProblem(parsed.values, parsed.missing), /Could not read/);
});

test('refuses secure boot or flash-encrypted boards before USB writes', () => {
  for (const [field, activeValue] of [
    ['SECURE_BOOT_EN', 'True'],
    ['SPI_BOOT_CRYPT_CNT', 'Enable']
  ]) {
    const parsed = parseUsbFlashEfuses(openFuseSummary.replace(new RegExp(`^${field}.*$`, 'm'), `${field} (BLOCK0) = ${activeValue}`));
    assert.match(usbFlashEfuseProblem(parsed.values, parsed.missing), new RegExp(`${field} is active`));
  }
});

test('refuses a board when all USB ROM download paths are disabled', () => {
  const blocked = openFuseSummary
    .replace('DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE (BLOCK0) Disable USB download = False R/W (0b0)', 'DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE (BLOCK0) Disable USB download = True R/W (0b1)')
    .replace('DIS_USB_OTG_DOWNLOAD_MODE (BLOCK0) Disable USB OTG download = False R/W (0b0)', 'DIS_USB_OTG_DOWNLOAD_MODE (BLOCK0) Disable USB OTG download = True R/W (0b1)');
  const parsed = parseUsbFlashEfuses(blocked);
  assert.match(usbFlashEfuseProblem(parsed.values, parsed.missing), /Both USB download paths are disabled/);
});

test('release and USB flashing paths never burn eFuses or request encrypted writes', () => {
  const toolsDirectory = import.meta.dirname;
  const files = [
    'release-esp-idf-firmware.mjs',
    'create-launchpad-flash-package.mjs',
    '../usb-flash-package/flash.py',
    '../usb-flash-package/flash-macos.command',
    '../usb-flash-package/flash-windows.bat'
  ];
  const sources = files.map((file) => fs.readFileSync(path.resolve(toolsDirectory, file), 'utf8'));
  const joined = sources.join('\n');

  assert.match(sources[2], /"summary"/);
  assert.doesNotMatch(joined, /\b(?:burn[_-]?(?:efuse|key)|write[_-]?efuse)\b|--encrypt\b/i);
});
