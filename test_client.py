#!/usr/bin/env python3
"""
Minimal BLE OTA test client using bleak.
Sends a .bin file to the ESP32-C3 recovery firmware with optional signature.
"""

import asyncio
import sys
import struct
import hashlib
from pathlib import Path
from bleak import BleakClient, BleakScanner

OTA_SERVICE_UUID = "0000180a-0000-1000-8000-00805f9b34fb"
OTA_CHAR_UUID = "00002a57-0000-1000-8000-00805f9b34fb"
CHUNK_SIZE = 240
RETRY_COUNT = 3
RETRY_DELAY = 0.5

def load_private_key(key_path):
    """Load ECDSA private key for signing."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    with open(key_path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)

def sign_firmware(firmware_data, private_key):
    """Sign firmware with ECDSA P-256."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
    digest = hashlib.sha256(firmware_data).digest()
    sig = private_key.sign(digest, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(sig)
    return r.to_bytes(32, 'big') + s.to_bytes(32, 'big')

def decode_dss_signature(sig):
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    return decode_dss_signature(sig)

async def find_device(timeout=10.0):
    print(f"Scanning for OTA-Recovery device...")
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name and "OTA-Recovery" in d.name:
            print(f"Found: {d.name} ({d.address})")
            return d.address
    print("Device not found")
    return None

async def send_firmware(address: str, firmware_path: Path, signature: bytes = None):
    firmware_data = firmware_path.read_bytes()
    firmware_size = len(firmware_data)
    print(f"Firmware: {firmware_path.name} ({firmware_size} bytes)")
    
    sha256 = hashlib.sha256(firmware_data).hexdigest()
    print(f"SHA-256: {sha256}")
    
    if signature:
        print(f"Signature: {len(signature)} bytes")
    
    async with BleakClient(address) as client:
        print("Connected")
        
        # Send header: 4 bytes size + 1 byte sig_size
        sig_size = len(signature) if signature else 0
        header = struct.pack("<IB", firmware_size, sig_size)
        for attempt in range(RETRY_COUNT):
            try:
                await client.write_gatt_char(OTA_CHAR_UUID, header, response=True)
                print("Header sent")
                break
            except Exception as e:
                if attempt == RETRY_COUNT - 1:
                    raise
                print(f"Header send failed, retry {attempt+1}: {e}")
                await asyncio.sleep(RETRY_DELAY)
        
        # Send firmware in chunks
        total_sent = 0
        chunk_num = 0
        
        while total_sent < firmware_size:
            chunk = firmware_data[total_sent:total_sent + CHUNK_SIZE]
            chunk_num += 1
            
            for attempt in range(RETRY_COUNT):
                try:
                    await client.write_gatt_char(OTA_CHAR_UUID, chunk, response=True)
                    break
                except Exception as e:
                    if attempt == RETRY_COUNT - 1:
                        raise
                    print(f"Chunk {chunk_num} failed, retry {attempt+1}: {e}")
                    await asyncio.sleep(RETRY_DELAY)
            
            total_sent += len(chunk)
            progress = (total_sent / firmware_size) * 100
            print(f"\rProgress: {progress:.1f}% ({total_sent}/{firmware_size})", end="", flush=True)
        
        # Send signature if provided
        if signature:
            sig_sent = 0
            while sig_sent < len(signature):
                chunk = signature[sig_sent:sig_sent + CHUNK_SIZE]
                for attempt in range(RETRY_COUNT):
                    try:
                        await client.write_gatt_char(OTA_CHAR_UUID, chunk, response=True)
                        break
                    except Exception as e:
                        if attempt == RETRY_COUNT - 1:
                            raise
                        print(f"Sig chunk failed, retry {attempt+1}: {e}")
                        await asyncio.sleep(RETRY_DELAY)
                sig_sent += len(chunk)
                print(f"\rSignature: {sig_sent}/{len(signature)} bytes", end="", flush=True)
            print()
        
        print("\nTransfer complete, waiting for device to verify and reboot...")
        await asyncio.sleep(5)

async def main():
    if len(sys.argv) < 2:
        print("Usage: python test_client.py <firmware.bin> [private_key.pem]")
        sys.exit(1)
    
    firmware_path = Path(sys.argv[1])
    if not firmware_path.exists():
        print(f"File not found: {firmware_path}")
        sys.exit(1)
    
    signature = None
    if len(sys.argv) > 2:
        key_path = Path(sys.argv[2])
        if key_path.exists():
            print(f"Signing with {key_path}...")
            private_key = load_private_key(key_path)
            firmware_data = firmware_path.read_bytes()
            signature = sign_firmware(firmware_data, private_key)
            print(f"Signature generated ({len(signature)} bytes)")
        else:
            print(f"Key file not found: {key_path}")
            sys.exit(1)
    
    address = await find_device()
    if not address:
        sys.exit(1)
    
    try:
        await send_firmware(address, firmware_path, signature)
        print("Success!")
    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())