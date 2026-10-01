#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MANIFEST_PATH = ROOT / "manifest.json"
EFUSE_FIELDS = [
    "SECURE_BOOT_EN",
    "SPI_BOOT_CRYPT_CNT",
    "DIS_USB_SERIAL_JTAG",
    "DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE",
    "DIS_USB_OTG_DOWNLOAD_MODE",
]


def fail(message):
    raise RuntimeError(message)


def read_manifest():
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if manifest.get("formatVersion") != 1 or manifest.get("product") != "ludant-esp32s3":
        fail("This is not a supported Ludant USB firmware package.")
    if manifest.get("chip") != "esp32s3" or manifest.get("hardware") != "ESP32-S3":
        fail("This package is not for an ESP32-S3 controller.")
    if not re.fullmatch(r"\d+\.\d+\.\d+", manifest.get("firmwareVersion", "")):
        fail("The package has an invalid firmware version.")
    if not manifest.get("bootloader") or len(manifest.get("projectImages", [])) < 3:
        fail("The package is missing required first-flash images.")

    entries = [manifest["bootloader"], *manifest["projectImages"]]
    seen_offsets = set()
    for entry in entries:
        relative = Path(entry.get("path", ""))
        if relative.is_absolute() or ".." in relative.parts or not entry.get("offset", "").startswith("0x"):
            fail("The package contains an unsafe flash image reference.")
        if entry["offset"] in seen_offsets:
            fail("The package contains duplicate flash offsets.")
        seen_offsets.add(entry["offset"])
        image_path = (ROOT / relative).resolve()
        if ROOT not in image_path.parents or not image_path.is_file():
            fail(f"Required image is missing: {relative}")
        data = image_path.read_bytes()
        if len(data) != entry.get("size") or hashlib.sha256(data).hexdigest() != entry.get("sha256"):
            fail(f"Image verification failed: {relative}")

    names = {Path(entry["path"]).name for entry in entries}
    if not {"bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "ludant_esp32s3_ota.bin"}.issubset(names):
        fail("The package does not contain the complete ESP-IDF flash image set.")
    return manifest, entries


def esptool_prefix():
    if importlib.util.find_spec("esptool") is None:
        fail("esptool is not installed. Install Python 3.10 or later, then run: python3 -m pip install esptool (Mac) or py -3 -m pip install esptool (Windows).")
    return [sys.executable, "-m", "esptool"]


def espefuse_command():
    found = shutil.which("espefuse") or shutil.which("espefuse.exe")
    if found:
        return [found]
    scripts_dir = Path(sysconfig.get_path("scripts"))
    for name in ("espefuse", "espefuse.exe", "espefuse-script.py"):
        candidate = scripts_dir / name
        if candidate.exists():
            return [str(candidate)]
    fail("espefuse was not found. Install it with the same Python used for esptool: python3 -m pip install esptool (Mac) or py -3 -m pip install esptool (Windows).")


def run(command, capture=False):
    result = subprocess.run(command, text=True, capture_output=capture)
    output = (result.stdout or "") + (result.stderr or "")
    if capture:
        return result.returncode, output
    if result.returncode != 0:
        fail(f"Command failed (exit {result.returncode}): {' '.join(command)}")
    return result.returncode, output


def list_ports():
    try:
        from serial.tools import list_ports
        return sorted(port.device for port in list_ports.comports())
    except ImportError:
        return []


def choose_port():
    ports = list_ports()
    if ports:
        print("Detected serial ports:")
        for index, port in enumerate(ports, 1):
            print(f"  {index}. {port}")
        if len(ports) == 1:
            answer = input(f"Use {ports[0]}? [Y/n] ").strip().lower()
            return ports[0] if answer in ("", "y", "yes") else input("Enter the board's serial port: ").strip()
        answer = input("Select the board's port number: ").strip()
        if answer.isdigit() and 1 <= int(answer) <= len(ports):
            return ports[int(answer) - 1]
        fail("Invalid port selection. No flash writes were made.")
    if os.name == "nt":
        print("Find the ESP32-S3 COM port in Windows Device Manager > Ports (COM & LPT).")
    else:
        print("Reconnect the board and find its port with: ls /dev/cu.*")
    port = input("Enter the board's serial port: ").strip()
    if not port:
        fail("No serial port selected. No flash writes were made.")
    return port


def parse_summary(output):
    values = {}
    for field in EFUSE_FIELDS:
        line = next((line for line in output.splitlines() if re.match(rf"\s*{re.escape(field)}\b", line)), None)
        if line:
            value = line.split("=")[-1].strip().split()[0]
            values[field] = value
    missing = [field for field in EFUSE_FIELDS if field not in values]
    if missing:
        fail(f"Could not read {', '.join(missing)}; refusing to flash. eFuse settings are irreversible.")

    active = lambda value: not re.fullmatch(r"false|0|0x0+|0b0+|disabled|disable", value, re.IGNORECASE)
    for field in ("SECURE_BOOT_EN", "SPI_BOOT_CRYPT_CNT"):
        if active(values[field]):
            fail(f"{field} is active; this board may require signed or encrypted firmware. No flash writes were made.")
    if active(values["DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE"]) and active(values["DIS_USB_OTG_DOWNLOAD_MODE"]):
        fail("Both USB ROM download paths are disabled by eFuse. No flash writes were made.")
    if active(values["DIS_USB_SERIAL_JTAG"]) and active(values["DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE"]):
        fail("USB Serial/JTAG and its ROM download mode are disabled by eFuse. No flash writes were made.")


def verify_board(port, manifest):
    prefix = esptool_prefix()
    common = ["--chip", manifest["chip"], "--port", port]
    code, output = run([*prefix, *common, "flash-id"], capture=True)
    if code != 0:
        fail(f"Could not communicate with the ESP32-S3. If needed, hold BOOT, tap RESET, then release BOOT.\n{output.strip()}")
    match = re.search(r"Detected flash size:\s*(\d+(?:\.\d+)?)\s*MB", output, re.IGNORECASE)
    if not match or float(match.group(1)) < 4:
        fail("Could not confirm at least 4 MB of flash. This firmware requires 4 MB; no flash writes were made.")
    print(f"Flash capacity: {match.group(1)} MB")

    command = [*espefuse_command(), "--chip", manifest["chip"], "--port", port, "summary", *EFUSE_FIELDS]
    code, output = run(command, capture=True)
    if code != 0:
        fail(f"Could not safely read the controller's eFuses. No flash writes were made.\n{output.strip()}")
    parse_summary(output)


def confirm(manifest, port):
    print("\nThis replaces the controller bootloader, partition table, OTA data, and application.")
    print("Do not flash a controller with Secure Boot or flash encryption enabled.")
    expected = f"FLASH LUDANT {manifest['firmwareVersion']} {port}"
    answer = input(f"Type exactly to continue:\n{expected}\n> ")
    if answer != expected:
        fail("Confirmation did not match. No flash writes were made.")


def image_args(entries):
    args = []
    for entry in entries:
        args.extend([entry["offset"], str((ROOT / entry["path"]).resolve())])
    return args


def flash(manifest, entries, port):
    settings = manifest["flashSettings"]
    prefix = esptool_prefix()
    connection = ["--chip", manifest["chip"], "--port", port, "--baud", "460800"]
    write_options = [
        "--flash-mode", settings["flash_mode"],
        "--flash-freq", settings["flash_freq"],
        "--flash-size", settings["flash_size"],
    ]
    print("\nWriting the bootloader…")
    run([*prefix, *connection, "--before", "default-reset", "--after", "no-reset", "--no-stub", "write-flash", *write_options, *image_args([entries[0]])])
    print("\nWriting the partition table, OTA data, and firmware…")
    run([*prefix, *connection, "--before", "default-reset", "--after", "hard-reset", "--no-stub", "write-flash", *write_options, *image_args(entries[1:])])


def main():
    if sys.version_info < (3, 10):
        fail("Python 3.10 or later is required for the current esptool release.")
    manifest, entries = read_manifest()
    print(f"Ludant ESP32-S3 firmware {manifest['firmwareVersion']}")
    port = choose_port()
    print(f"Selected port: {port}")
    verify_board(port, manifest)
    confirm(manifest, port)
    flash(manifest, entries, port)
    print("\nUSB flashing finished. Keep the board powered on, then return to Ludant and connect over Bluetooth.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, json.JSONDecodeError) as error:
        print(f"\nInstaller stopped: {error}", file=sys.stderr)
        sys.exit(1)
