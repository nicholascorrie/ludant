// Checks GCC -fstack-usage output from the actual ESP32-S3 sketch build.
// These budgets catch large local module arrays/buffers returning to loopTask.
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) {
  console.error('Usage: node check-arduino-stack.mjs <sketch.ino.cpp.su>');
  process.exit(2);
}

const budgets = new Map([
  ['handleModuleCommand', 512],
  ['loadModules', 512],
  ['processControlQueue', 256],
  ['processControlMessage', 512],
  ['parseModule', 1024],
]);
const found = new Set();
let failed = false;
for (const line of readFileSync(path, 'utf8').split('\n')) {
  const [signature, bytesText, kind] = line.split('\t');
  for (const [name, budget] of budgets) {
    if (!signature.includes(` ${name}(`)) continue;
    found.add(name);
    const bytes = Number(bytesText);
    const passes = Number.isFinite(bytes) && bytes <= budget && kind === 'static';
    console.log(`${passes ? 'PASS' : 'FAIL'} ${name}: ${bytes} bytes (budget ${budget})`);
    if (!passes) failed = true;
  }
}
for (const name of budgets.keys()) {
  if (!found.has(name)) {
    console.error(`FAIL: missing stack report for ${name}`);
    failed = true;
  }
}
process.exitCode = failed ? 1 : 0;
