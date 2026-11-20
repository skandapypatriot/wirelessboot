# ESP32-C3 Unbrickable BLE Recovery

A **bare-metal (`no_std`, zero RTOS)** Rust recovery firmware for the ESP32-C3
that lives in the `factory` partition and provides BLE OTA updates with an
**unbrickable guarantee** — even if the main Arduino app is completely erased
or corrupted, holding GPIO9 (BOOT) on power-up boots recovery.

---

## Partition Layout (4 MB flash)

| Partition  | Type   | Offset   | Size     | Purpose                          |
|------------|--------|----------|----------|----------------------------------|
| `nvs`      | data   | 0x9000   | 0x5000   | NVS storage                      |
| `otadata`  | data   | 0xE000   | 0x2000   | OTA boot select (2 entries)      |
| `app0`     | app    | 0x10000  | 0x180000 | Your Arduino app (ota_0)         |
| `factory`  | app    | 0x190000 | 0x180000 | **This recovery firmware**       |

**Key property:** Your Arduino app builds to `0x10000` and never touches
`0x190000`. Recovery is always safe.

---

## One-Line End-User Installer (PlatformIO Arduino)

Run inside any existing PlatformIO ESP32-C3 Arduino project folder:

```powershell
irm https://raw.githubusercontent.com/casmm/wirelessboot/main/install.ps1 | iex
```

Or with a specific serial port:

```powershell
irm https://raw.githubusercontent.com/casmm/wirelessboot/main/install.ps1 | iex -Port COM3
```

What `install.ps1` does automatically:

1. Verifies `platformio.ini` exists in the current directory
2. Downloads `partitions.csv`, `bootloader.bin`, `recovery.bin`, `ota_client.py`
   into a `.wirelessboot/` subfolder
3. Backs up `platformio.ini` to `platformio.ini.bak` and patches it to use
   `board_build.partitions = .wirelessboot/partitions.csv`
4. Locates PlatformIO's `esptool.py` and flashes:
   - `bootloader.bin` → `0x0`
   - `partitions.csv` → `0x8000`
   - `recovery.bin`   → `0x190000`

After installing, build and upload your Arduino app normally. It goes to `ota_0`
(`0x10000`). Recovery stays at `factory` (`0x190000`).

---

## Triggering Recovery

| Condition                              | Result                              |
|----------------------------------------|-------------------------------------|
| GPIO9 (BOOT) held on power-up          | → Recovery mode (BLE advertising)   |
| `ota_0` missing `0xE9` image header    | → Recovery mode (auto-fallback)     |
| `otadata` empty/invalid                | → Bootloader falls back to `factory`|
| Normal boot, `ota_0` valid             | → Boots your Arduino app            |

---

## Uploading Firmware Over BLE

```bash
pip install bleak
python .wirelessboot/ota_client.py your_firmware.bin
```

The client:
1. Scans for `ESP32C3-Recovery`
2. Sends `BEGIN` (erases ota_0)
3. Streams firmware in 200-byte chunks with retry
4. Sends `END` (verifies `0xE9` header, switches boot to ota_0, resets)

On any failure, the device stays in recovery — `ota_0` is never left half-written.

---

## Developer Setup (Build from Source)

The Rust toolchain is installed **locally inside `.rust/`** — no global PATH
changes, no system modification.

### Prerequisites

- Python 3.12+ (for PlatformIO esptool.py)
- PlatformIO Core (`pip install platformio`)
- ESP32 platform installed (`pio platform install espressif32`)

### Bootstrap Local Rust Toolchain

```powershell
.\install-rust.ps1
```

This downloads `rustup-init.exe`, installs the stable toolchain with minimal
profile into `.rust/`, adds the `riscv32imc-unknown-none-elf` target, and adds
`llvm-tools-preview` (provides `dlltool.exe` needed on Windows).

### Build the Recovery Firmware

```powershell
.\cargo_local.ps1 build --release --target riscv32imc-unknown-none-elf
```

The ELF output goes to `target/riscv32imc-unknown-none-elf/release/`.

### Convert ELF to Binary and Flash

```powershell
# Convert ELF to ESP32 image
python $env:USERPROFILE\.platformio\packages\tool-esptool\esptool.py --chip esp32c3 elf2image \
  -o release/recovery.bin target/riscv32imc-unknown-none-elf/release/wirelessboot-recovery

# Flash to factory partition (0x190000)
python $env:USERPROFILE\.platformio\packages\tool-esptool\esptool.py --chip esp32c3 write_flash \
  0x190000 release/recovery.bin
```

---

## Project Structure

```
wirelessboot/
├── install-rust.ps1          # Bootstrap local Rust toolchain (.rust/)
├── cargo_local.ps1           # Run cargo with local toolchain (no PATH pollution)
├── install.ps1               # One-line GitHub installer for Arduino users
├── platformio.ini            # ESP-IDF build config (fallback)
├── Cargo.toml                # Rust crate manifest
├── src/main.rs               # Bare-metal recovery firmware (no_std, no_main)
├── release/
│   ├── partitions.csv        # Partition table (factory at 0x190000)
│   ├── bootloader.bin        # 2nd-stage bootloader (GPIO9 trigger)
│   ├── recovery.bin          # Prebuilt recovery firmware
│   └── ota_client.py         # BLE upload client (Python + bleak)
├── .rust/                    # Local Rust toolchain (gitignored)
├── .pio/                     # PlatformIO build cache (gitignored)
├── target/                   # Rust build output (gitignored)
└── README.md
```

---

## How It Works (Bare-Metal Rust)

1. **`src/main.rs`** — `#![no_std]`, `#![no_main]` entry point
2. **GPIO9 check** — Reads BOOT button with internal pull-up
3. **`ota_0` validation** — Reads first byte at `0x10000`, checks for `0xE9` ESP32 image magic
4. **otadata update** — Writes `esp_ota_select_entry_t` (seq + state + CRC32) to `0xE000`
5. **Software reset** — Jumps to `ota_0` (normal boot) or stays in recovery
6. **BLE GATT server** — Advertises `ESP32C3-Recovery`, Nordic UART Service, RX characteristic
7. **Flash writer** — `esp-storage` buffered sector writer for `0x10000`

---

## Size Targets

| Metric | Target |
|--------|--------|
| Recovery binary | < 256 KB (fits factory partition) |
| RAM at runtime | < 60 KB |
| User app flash impact | 0 bytes (separate partition) |
| User app RAM impact | 0 bytes (recovery is separate app slot) |

---

## Unverified Items

- [ ] Actual `cargo build --release` binary size on Windows (dlltool issue with windows-sys)
- [ ] BLE throughput / transfer time
- [ ] Real hardware testing on ESP32-C3
- [ ] Bootloader with GPIO9 factory reset trigger (needs ESP-IDF bootloader build)
- [ ] Power-loss-during-upload recovery testing
- [ ] Signature verification (placeholder)

---

## License

MIT OR Apache-2.0