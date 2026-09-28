#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import readline from 'node:readline/promises';
import {
  assertUnlockedConfiguration,
  checkUsbFlashEfuses,
  disableIrreversibleSecurityOptions
} from './unlocked-firmware-safety.mjs';

const firmwareRoot = path.resolve(import.meta.dirname, '..');
const sdkconfigPath = path.join(firmwareRoot, 'sdkconfig');
const sdkconfigHeaderPath = path.join(firmwareRoot, 'build/config/sdkconfig.h');

function parseArguments(args) {
  const options = { monitor: false, port: undefined };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === '--monitor') {
      options.monitor = true;
      continue;
    }
    if (argument === '--port') {
      const value = args[index + 1];
      if (!value || value.startsWith('--')) throw new Error('--port requires a serial device path');
      options.port = value;
      index += 1;
      continue;
    }
    if (argument === '--help' || argument === '-h') {
      console.log('Usage: npm run firmware:flash:unlocked -- [--port <serial-port>] [--monitor]');
      process.exit(0);
    }
    throw new Error(`Unknown argument: ${argument}`);
  }
  return options;
}

function listSerialPorts() {
  return fs.readdirSync('/dev')
    .filter((name) => /^cu\.(?:usb|SLAB|wchusbserial|usbserial)/i.test(name))
    .map((name) => path.join('/dev', name))
    .sort();
}

function selectPort(requestedPort) {
  const ports = listSerialPorts();
  if (requestedPort) {
    if (!fs.existsSync(requestedPort)) {
      throw new Error(`Serial port not found: ${requestedPort}${ports.length ? `\nAvailable USB serial ports:\n${ports.join('\n')}` : ''}`);
    }
    return requestedPort;
  }
  if (ports.length === 1) return ports[0];
  if (ports.length === 0) throw new Error('No USB serial ports found. Connect the ESP32-S3 with a data-capable USB cable.');
  throw new Error(`Multiple USB serial ports found; rerun with --port <path>:\n${ports.join('\n')}`);
}

function runIdf(args) {
  const result = spawnSync('idf.py', args, { cwd: firmwareRoot, env: process.env, stdio: 'inherit' });
  if (result.error) throw new Error(`Could not run idf.py: ${result.error.message}`);
  if (result.status !== 0) throw new Error(`idf.py ${args.join(' ')} failed with exit code ${result.status}`);
}

async function confirmFlash(port) {
  if (!process.stdin.isTTY) throw new Error('A terminal is required to confirm replacing the board firmware.');
  const expected = `FLASH UNLOCKED FIRMWARE ${port}`;
  console.log(`About to build and replace the firmware on ${port}.`);
  console.log('The build will keep Secure Boot, flash encryption, NVS encryption, and hardware anti-rollback disabled.');
  const terminal = readline.createInterface({ input: process.stdin, output: process.stdout });
  try {
    const answer = await terminal.question(`Type exactly:\n${expected}\n> `);
    if (answer !== expected) throw new Error('Confirmation did not match; no flash writes were made.');
  } finally {
    terminal.close();
  }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const port = selectPort(options.port);
  console.log(`Selected serial port: ${port}`);

  // Read irreversible device state before building or allowing idf.py flash.
  const fuseSummary = checkUsbFlashEfuses(port, { cwd: firmwareRoot });
  console.log(fuseSummary.trim());

  const existingConfig = fs.existsSync(sdkconfigPath)
    ? fs.readFileSync(sdkconfigPath, 'utf8')
    : 'CONFIG_IDF_TARGET="esp32s3"\n';
  const safeConfig = disableIrreversibleSecurityOptions(existingConfig);
  fs.writeFileSync(sdkconfigPath, safeConfig);
  assertUnlockedConfiguration(fs.readFileSync(sdkconfigPath, 'utf8'), 'sdkconfig');

  runIdf(['build']);
  if (!fs.existsSync(sdkconfigHeaderPath)) throw new Error('ESP-IDF did not generate build/config/sdkconfig.h');
  assertUnlockedConfiguration(fs.readFileSync(sdkconfigHeaderPath, 'utf8'), 'generated build configuration');
  await confirmFlash(port);
  runIdf(['-p', port, ...(options.monitor ? ['flash', 'monitor'] : ['flash'])]);
}

main().catch((error) => {
  console.error(`Unlocked flash stopped: ${error.message}`);
  process.exitCode = 1;
});
