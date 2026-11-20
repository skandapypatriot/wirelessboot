# BLE OTA Recovery Firmware for ESP32-C3

A minimal factory partition firmware (<250 KB) that provides BLE-based OTA recovery.
Normal user apps contain zero BLE code.

## Hardware
- ESP32-C3, 4 MB flash
- BOOT button on GPIO9 (active low)

## Partition Table
| Name    | Type | SubType | Offset  | Size    |
|---------|------|---------|---------|---------|
| nvs     | data | nvs     | 0x9000  | 0x4000  |
| otadata | data | ota     | 0xD000  | 0x2000  |
| factory | app  | factory | 0x10000 | 0x40000 (256 KB) |
| ota_0   | app  | ota_0   | 0x50000 | 0x3B0000 |

## Build
```bash
# Requires ESP-IDF v5.4+
idf.py set-target esp32c3
idf.py build
idf.py size
idf.py size-components
```

## Flash
```bash
idf.py flash
# Or manually:
# esptool.py --chip esp32c3 write_flash 0x10000 build/ble_ota_recovery.bin
```

## Operation
1. **Normal boot**: BOOT button not pressed → immediately boots `ota_0`
2. **Recovery mode**: Hold BOOT (GPIO9) at power-on → starts BLE advertising as "OTA-Recovery"
3. **OTA transfer**: Connect via BLE, write firmware size (4 bytes LE) + chunks to characteristic 0x2A57
4. **Verification**: On disconnect, validates SHA-256, writes to `ota_0`, sets boot partition, reboots
5. **Failure**: Any error → stays in recovery, `ota_0` untouched

## GATT Service
- Service UUID: `0x180A` (Device Information)
- Characteristic UUID: `0x2A57` (Digital Output) - Write/Write-No-Response
- First write: 4-byte little-endian total firmware size
- Subsequent writes: firmware chunks (max 240 bytes each)
- Flow control: client waits for write response (ACK)

## Test Client
```bash
pip install bleak
python test_client.py firmware.bin
```

## Size Optimization (Phase 1 → 2)
| Phase | Config Change | Binary Size | Delta |
|-------|--------------|-------------|-------|
| 1 | Baseline (defaults) | TBD | - |
| 2 | CONFIG_COMPILER_OPTIMIZATION_SIZE=y | TBD | TBD |
| 2 | CONFIG_LOG_DEFAULT_LEVEL_NONE=y | TBD | TBD |
| 2 | BT roles: peripheral only | TBD | TBD |
| 2 | Max connections = 1 | TBD | TBD |
| 2 | Disable WiFi, Classic BT, Mesh | TBD | TBD |
| 2 | Minimal NimBLE buffers | TBD | TBD |
| 2 | Disable assertions, checks | TBD | TBD |
| 2 | Nano newlib, no float printf | TBD | TBD |
| **Target** | **< 250 KB (256 KB partition)** | **< 256000** | |

## RAM Usage (Target < 60 KB)
| Component | Est. RAM |
|-----------|----------|
| FreeRTOS tasks (2x 4KB) | ~8 KB |
| NimBLE host stack | ~15 KB |
| BT controller | ~20 KB |
| OTA buffers + SHA-256 | ~5 KB |
| **Total** | **~48 KB** |

## Test Results
| Test | Result |
|------|--------|
| Successful upload | TBD |
| Corrupted image rejection | TBD |
| Power loss during upload recovery | TBD |
| Normal boot (no BLE init) | TBD |
| Button press detection | TBD |

## Unverified Items
- [ ] Actual binary size after `idf.py size`
- [ ] Actual RAM usage at runtime
- [ ] BLE throughput / transfer time
- [ ] Signature verification (placeholder in code)
- [ ] Rollback on failed verification
- [ ] Deep sleep / power management

## Next Steps (Phases 3-6)
- Phase 3: Add signature verification (ECDSA P-256)
- Phase 4: Bootloader hook for faster button check
- Phase 5: Complete test_client.py with progress/retry
- Phase 6: No-FreeRTOS variant (only if Phase 2 > 250 KB)