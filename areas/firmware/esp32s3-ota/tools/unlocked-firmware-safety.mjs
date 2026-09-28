import { spawnSync } from 'node:child_process';

export const SECURITY_OPTIONS_THAT_MUST_REMAIN_OFF = [
  'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK',
  'CONFIG_APP_ANTI_ROLLBACK',
  'CONFIG_SECURE_BOOT',
  'CONFIG_SECURE_BOOT_V1_ENABLED',
  'CONFIG_SECURE_BOOT_V2_ENABLED',
  'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES',
  'CONFIG_SECURE_FLASH_ENC_ENABLED',
  'CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT',
  'CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE',
  'CONFIG_FLASH_ENCRYPTION_ENABLED',
  'CONFIG_FLASH_ENCRYPTION_INSECURE',
  'CONFIG_FLASH_ENCRYPTION_UART_BOOTLOADER_ALLOW_ENCRYPT',
  'CONFIG_SECURE_FLASH_UART_BOOTLOADER_ALLOW_ENC',
  'CONFIG_SECURE_INSECURE_ALLOW_DL_MODE',
  'CONFIG_NVS_ENCRYPTION',
  'CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC',
  'CONFIG_SECURE_DISABLE_ROM_DL_MODE',
  'CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE'
];

export const USB_FLASH_EFUSE_FIELDS = [
  'SECURE_BOOT_EN',
  'SPI_BOOT_CRYPT_CNT',
  'DIS_USB_SERIAL_JTAG',
  'DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE',
  'DIS_USB_OTG_DOWNLOAD_MODE'
];

export function disableIrreversibleSecurityOptions(config) {
  let safeConfig = config;
  for (const key of SECURITY_OPTIONS_THAT_MUST_REMAIN_OFF) {
    const escaped = key.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    safeConfig = safeConfig.replace(new RegExp(`^(?:${escaped}=.*|# ${escaped} is not set)\\r?\\n?`, 'gm'), '');
    safeConfig = `${safeConfig.trimEnd()}\n# ${key} is not set\n`;
  }
  return safeConfig;
}

export function findEnabledSecurityOptions(configOrHeader) {
  const lines = new Set(configOrHeader.split(/\r?\n/));
  return SECURITY_OPTIONS_THAT_MUST_REMAIN_OFF.filter((key) =>
    lines.has(`${key}=y`) || lines.has(`#define ${key} 1`)
  );
}

export function assertUnlockedConfiguration(configOrHeader, source = 'firmware configuration') {
  const enabled = findEnabledSecurityOptions(configOrHeader);
  if (enabled.length) {
    throw new Error(`${source} enables settings that can permanently restrict owner reflashing: ${enabled.join(', ')}`);
  }
}

export function parseUsbFlashEfuses(output, fields = USB_FLASH_EFUSE_FIELDS) {
  const lines = output.split(/\r?\n/);
  const values = {};
  const missing = [];
  for (const field of fields) {
    const line = lines.find((candidate) => new RegExp(`^\\s*${field}\\b`).test(candidate));
    const value = line?.split('=').at(-1)?.trim().split(/\s+/)[0];
    if (!value) missing.push(field);
    else values[field] = value;
  }
  return { values, missing };
}

function fuseIsActive(value) {
  return !/^(?:false|0|0x0+|0b0+|disabled|disable)$/i.test(value ?? '');
}

export function usbFlashEfuseProblem(values, missing = []) {
  if (missing.length) return `Could not read ${missing.join(', ')}; refusing to flash.`;
  for (const field of ['SECURE_BOOT_EN', 'SPI_BOOT_CRYPT_CNT']) {
    if (fuseIsActive(values[field])) {
      return `${field} is active (${values[field]}); this board may require signed or encrypted images and cannot be restored to an unlocked state.`;
    }
  }

  const usbDownloadAvailable = ['DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE', 'DIS_USB_OTG_DOWNLOAD_MODE']
    .some((field) => !fuseIsActive(values[field]));
  if (!usbDownloadAvailable) {
    return 'Both USB download paths are disabled by eFuse; owner USB flashing is unavailable.';
  }
  if (fuseIsActive(values.DIS_USB_SERIAL_JTAG) && fuseIsActive(values.DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE)) {
    return 'USB Serial/JTAG is disabled and its ROM download mode is unavailable by eFuse.';
  }
  return null;
}

export function checkUsbFlashEfuses(port, { cwd, env = process.env, run = spawnSync } = {}) {
  const result = run('espefuse', [
    '--chip', 'esp32s3', '--port', port, 'summary', ...USB_FLASH_EFUSE_FIELDS
  ], {
    cwd,
    env,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe']
  });
  if (result.error) throw new Error(`Could not read ESP32-S3 eFuses: ${result.error.message}`);
  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`;
  if (result.status !== 0) {
    throw new Error(`Could not read ESP32-S3 eFuses (espefuse exit ${result.status}):\n${output.trim()}`);
  }
  const parsed = parseUsbFlashEfuses(output);
  const problem = usbFlashEfuseProblem(parsed.values, parsed.missing);
  if (problem) throw new Error(`${problem}\nNo flash writes were made. eFuse settings are irreversible.`);
  return output;
}
