import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export function beginUsbFlashAssetTransaction(assetsDirectory, launchpadDirectory = null) {
  const backupDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'ludant-usb-assets-'));
  const launchpadBackupPath = path.join(backupDirectory, 'launchpad');
  const assets = fs.existsSync(assetsDirectory)
    ? fs.readdirSync(assetsDirectory).filter((name) => name.endsWith('-usb.zip'))
    : [];
  const hadLaunchpadDirectory = launchpadDirectory != null && fs.existsSync(launchpadDirectory);
  let completed = false;

  const moved = [];
  try {
    for (const name of assets) {
      fs.renameSync(path.join(assetsDirectory, name), path.join(backupDirectory, name));
      moved.push(name);
    }
    if (hadLaunchpadDirectory) fs.renameSync(launchpadDirectory, launchpadBackupPath);
  } catch (error) {
    for (const name of moved) {
      fs.renameSync(path.join(backupDirectory, name), path.join(assetsDirectory, name));
    }
    if (hadLaunchpadDirectory && fs.existsSync(launchpadBackupPath)) {
      fs.renameSync(launchpadBackupPath, launchpadDirectory);
    }
    fs.rmSync(backupDirectory, { recursive: true, force: true });
    throw error;
  }

  return {
    commit() {
      if (completed) return;
      completed = true;
      fs.rmSync(backupDirectory, { recursive: true, force: true });
    },
    rollback() {
      if (completed) return;
      completed = true;
      if (fs.existsSync(assetsDirectory)) {
        for (const name of fs.readdirSync(assetsDirectory).filter((entry) => entry.endsWith('-usb.zip'))) {
          fs.rmSync(path.join(assetsDirectory, name), { force: true });
        }
      } else {
        fs.mkdirSync(assetsDirectory, { recursive: true });
      }
      for (const name of assets) {
        const backupPath = path.join(backupDirectory, name);
        if (fs.existsSync(backupPath)) fs.renameSync(backupPath, path.join(assetsDirectory, name));
      }
      if (launchpadDirectory) {
        fs.rmSync(launchpadDirectory, { recursive: true, force: true });
        if (hadLaunchpadDirectory && fs.existsSync(launchpadBackupPath)) {
          fs.mkdirSync(path.dirname(launchpadDirectory), { recursive: true });
          fs.renameSync(launchpadBackupPath, launchpadDirectory);
        }
      }
      fs.rmSync(backupDirectory, { recursive: true, force: true });
    }
  };
}
