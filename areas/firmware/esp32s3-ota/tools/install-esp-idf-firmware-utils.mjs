const ANSI_ESCAPE = /\u001b\[[0-?]*[ -/]*[@-~]/g;

export function selectCatalogRelease(catalog, requestedVersion) {
  const releases = (catalog.versions ?? []).filter((release) => /^(?:\d+)\.(?:\d+)\.(?:\d+)$/.test(release.version));
  if (requestedVersion) {
    const explicit = releases.find((release) => release.version === requestedVersion);
    if (!explicit) throw new Error(`Version ${requestedVersion} is not listed in FirmwareCatalog.json`);
    return explicit;
  }

  const latest = catalog.latest;
  if (!latest || typeof latest.version !== 'string' || typeof latest.asset !== 'string') {
    throw new Error('FirmwareCatalog.json has no latest release pointer; create/update a release before installing.');
  }
  const release = releases.find((candidate) => candidate.version === latest.version);
  if (!release || release.asset !== latest.asset) {
    throw new Error(`FirmwareCatalog.json latest pointer does not match a release entry: ${latest.version} (${latest.asset})`);
  }
  return release;
}

export function parseFuseSummary(output, fields) {
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

export function lockedFlashReason(values) {
  for (const field of ['SECURE_BOOT_EN', 'SPI_BOOT_CRYPT_CNT']) {
    const value = values[field];
    if (value === undefined) return `Could not read ${field}; refusing to flash.`;
    const isOff = /^(?:false|0|0x0+|0b0+|disabled|disable)$/i.test(value);
    if (!isOff) {
      return `${field} is already active (${value}); this board may require signed or encrypted images and cannot be restored to an unlocked state.`;
    }
  }
  return null;
}

export function inspectBootLog(output, expectedVersion) {
  const clean = output.replace(ANSI_ESCAPE, '');
  if (/Guru Meditation|InstrFetchProhibited|LoadProhibited|StoreProhibited|abort\(\) was called|ESP_ERROR_CHECK failed|assert failed|rebooting/i.test(clean)) {
    return 'crashed';
  }
  const readyVersion = clean.match(/Firmware ready; version=([^\s]+)/)?.[1];
  if (expectedVersion && readyVersion && readyVersion !== expectedVersion) return 'version-mismatch';
  if (/BLE advertising as Ludant\b/.test(clean) && readyVersion === expectedVersion) return 'ready';
  return 'pending';
}
