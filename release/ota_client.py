#!/usr/bin/env python3
"""
BLE OTA Client for ESP32-C3 Unbrickable Recovery.

Uploads a firmware .bin to the recovery partition via BLE.
The recovery firmware advertises as "ESP32C3-Recovery" with
Nordic UART Service (NUS) UUID.

Usage:
    python ota_client.py <firmware.bin> [--port COMx] [--addr XX:XX:XX:XX:XX:XX]

Protocol (writes to NUS RX characteristic 6E400002-...):
    1. "BEGIN"         - Erase ota_0 and reset write cursor
    2. Binary chunks   - Written sequentially to flash at 0x10000
    3. "END"           - Finalize, verify 0xE9 header, switch boot to ota_0, reset
"""

import asyncio
import sys
import struct
import hashlib
import argparse
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("bleak not installed. Run: pip install bleak")
    sys.exit(1)

# Nordic UART Service (NUS) UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Write (from client)

CHUNK_SIZE = 200  # BLE MTU-safe chunk size
RETRY_COUNT = 3
RETRY_DELAY = 0.5


async def find_device(timeout=10.0, address=None):
    """Scan for ESP32C3-Recovery device or connect to specific address."""
    if address:
        print(f"Connecting to {address}...")
        return address

    print(f"Scanning for ESP32C3-Recovery (timeout={timeout}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        name = d.name or ""
        if "ESP32C3-Recovery" in name or "Recovery" in name:
            print(f"Found: {name} ({d.address})")
            return d.address

    print("Device not found. Make sure the ESP32-C3 is in recovery mode (BOOT button held on power-up).")
    return None


async def upload_firmware(address: str, firmware_path: Path):
    """Upload firmware via BLE OTA."""
    firmware_data = firmware_path.read_bytes()
    firmware_size = len(firmware_data)

    print(f"Firmware: {firmware_path.name}")
    print(f"Size: {firmware_size} bytes ({firmware_size / 1024:.1f} KB)")

    sha256 = hashlib.sha256(firmware_data).hexdigest()
    print(f"SHA-256: {sha256}")

    async with BleakClient(address) as client:
        print("Connected to recovery device")

        # Step 1: Send BEGIN
        print("Sending BEGIN command...")
        for attempt in range(RETRY_COUNT):
            try:
                await client.write_gatt_char(NUS_RX_UUID, b"BEGIN", response=True)
                print("  BEGIN acknowledged")
                break
            except Exception as e:
                if attempt == RETRY_COUNT - 1:
                    raise
                print(f"  BEGIN failed, retry {attempt + 1}: {e}")
                await asyncio.sleep(RETRY_DELAY)

        # Step 2: Send binary chunks
        print(f"Uploading {firmware_size} bytes in {CHUNK_SIZE}-byte chunks...")
        total_sent = 0
        chunk_num = 0

        while total_sent < firmware_size:
            chunk = firmware_data[total_sent:total_sent + CHUNK_SIZE]
            chunk_num += 1

            for attempt in range(RETRY_COUNT):
                try:
                    await client.write_gatt_char(NUS_RX_UUID, chunk, response=True)
                    break
                except Exception as e:
                    if attempt == RETRY_COUNT - 1:
                        raise
                    print(f"  Chunk {chunk_num} failed, retry {attempt + 1}: {e}")
                    await asyncio.sleep(RETRY_DELAY)

            total_sent += len(chunk)
            progress = (total_sent / firmware_size) * 100
            print(f"\r  Progress: {progress:6.1f}% ({total_sent:>7}/{firmware_size})", end="", flush=True)

        print()  # newline after progress

        # Step 3: Send END
        print("Sending END command...")
        for attempt in range(RETRY_COUNT):
            try:
                await client.write_gatt_char(NUS_RX_UUID, b"END", response=True)
                print("  END acknowledged")
                break
            except Exception as e:
                if attempt == RETRY_COUNT - 1:
                    raise
                print(f"  END failed, retry {attempt + 1}: {e}")
                await asyncio.sleep(RETRY_DELAY)

        print("\nUpload complete! Device should verify and reboot into the new firmware.")
        print("SHA-256:", sha256)


async def main():
    parser = argparse.ArgumentParser(description="BLE OTA Client for ESP32-C3 Recovery")
    parser.add_argument("firmware", help="Path to firmware .bin file")
    parser.add_argument("--addr", help="BLE device address (skip scanning)")
    parser.add_argument("--timeout", type=float, default=10.0, help="Scan timeout in seconds")
    args = parser.parse_args()

    firmware_path = Path(args.firmware)
    if not firmware_path.exists():
        print(f"Error: File not found: {firmware_path}")
        sys.exit(1)

    address = await find_device(timeout=args.timeout, address=args.addr)
    if not address:
        sys.exit(1)

    try:
        await upload_firmware(address, firmware_path)
    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())