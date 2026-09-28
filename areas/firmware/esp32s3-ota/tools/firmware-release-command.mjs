#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../../../..');
const catalogPath = path.join(root, 'apps/ui/ludant-ios/ludant-ios/FirmwareCatalog.json');
const releaseTool = path.join(root, 'areas/firmware/esp32s3-ota/tools/release-esp-idf-firmware.mjs');

function usage() {
  console.error('Usage:');
  console.error('  npm run firmware:next');
  console.error('  npm run firmware:bump:patch [--dry-run]');
  console.error('  npm run firmware:bump:minor [--dry-run]');
  console.error('  npm run firmware:bump:major [--dry-run]');
  console.error('  npm run firmware:deploy -- <version> [--dry-run]');
  process.exit(2);
}

function readCatalog() {
  return JSON.parse(fs.readFileSync(catalogPath, 'utf8'));
}

function semver(value) {
  const match = String(value).match(/(?:^|-)(\d+)\.(\d+)\.(\d+)$/);
  return match ? match.slice(1).map(Number) : null;
}

function compare(left, right) {
  return left[0] - right[0] || left[1] - right[1] || left[2] - right[2];
}

function currentVersion(catalog) {
  const candidates = (catalog.versions ?? [])
    .map((release) => ({ value: release.version, parts: semver(release.version) }))
    .filter((release) => release.parts);
  if (candidates.length === 0) throw new Error('No semantic firmware version exists in FirmwareCatalog.json');
  return candidates.sort((left, right) => compare(right.parts, left.parts))[0];
}

function nextVersion(current, kind) {
  const [major, minor, patch] = current.parts;
  if (kind === 'major') return `${major + 1}.0.0`;
  if (kind === 'minor') return `${major}.${minor + 1}.0`;
  if (kind === 'patch') return `${major}.${minor}.${patch + 1}`;
  throw new Error(`Unknown version bump: ${kind}`);
}

function runRelease(version, dryRun) {
  const args = [releaseTool, version];
  if (dryRun) args.push('--dry-run');
  const result = spawnSync(process.execPath, args, { cwd: root, env: process.env, stdio: 'inherit' });
  if (result.error) throw result.error;
  process.exit(result.status ?? 1);
}

const argumentsList = process.argv.slice(2);
const command = argumentsList.shift();
const value = argumentsList.shift();
const extraArguments = argumentsList.filter((argument) => argument !== '--dry-run');
const dryRun = process.argv.includes('--dry-run');
const legacySecureVersion = extraArguments.length === 1 && /^\d+$/.test(extraArguments[0])
  ? extraArguments[0]
  : null;
const catalog = readCatalog();
const current = currentVersion(catalog);

if (command === 'next') {
  console.log(`Current firmware version: ${current.value}`);
  console.log(`Next patch version: ${nextVersion(current, 'patch')}`);
  process.exit(0);
}

if (command === 'bump') {
  if (extraArguments.length > 0 && legacySecureVersion === null) usage();
  const version = nextVersion(current, value);
  if (legacySecureVersion !== null) {
    console.warn(`Ignoring legacy secure-version argument ${legacySecureVersion}; open releases do not use eFuse anti-rollback.`);
  }
  console.log(`Releasing unlocked firmware ${version}`);
  runRelease(version, dryRun);
}

if (command === 'deploy') {
  if (!/^\d+\.\d+\.\d+$/.test(value ?? '') || extraArguments.length > 0) usage();
  runRelease(value, dryRun);
}

usage();
