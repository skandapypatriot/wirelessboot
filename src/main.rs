#![no_std]
#![no_main]

use core::sync::atomic::{AtomicBool, Ordering};

use crc32fast::Hasher;
use esp_backtrace as _;
use esp_hal::{
    clock::ClockControl,
    delay::Delay,
    gpio::{Input, Io, Pull},
    peripherals::Peripherals,
    prelude::*,
    reset::software_reset,
    system::SystemControl,
};
use esp_println::println;

// ──────────────────────────────────────────────────────────────
// Partition Layout (matches release/partitions.csv)
// ──────────────────────────────────────────────────────────────

const NVS_OFFSET: u32 = 0x9000;
const NVS_SIZE: u32 = 0x5000;

const OTADATA_OFFSET: u32 = 0xE000;
const OTADATA_SIZE: u32 = 0x2000;

const OTA0_OFFSET: u32 = 0x10000;
const OTA0_SIZE: u32 = 0x180000;

const FACTORY_OFFSET: u32 = 0x190000;
const FACTORY_SIZE: u32 = 0x180000;

const BOOT_BUTTON_GPIO: u8 = 9;

// ESP32 image magic byte (first byte of app image header)
const ESP_IMAGE_MAGIC: u8 = 0xE9;

// ──────────────────────────────────────────────────────────────
// otadata structure (ESP-IDF esp_ota_select_entry_t)
// ──────────────────────────────────────────────────────────────

#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
struct OtaSelectEntry {
    ota_seq: u32,
    ota_state: u8,
    padding: [u8; 3],
    crc: u32,
}

const OTA_STATE_VALID: u8 = 0x00;
const OTA_STATE_UNUSED: u8 = 0xFF;

fn calc_otadata_crc(seq: u32, state: u8, padding: &[u8; 3]) -> u32 {
    let mut hasher = Hasher::new();
    hasher.update(&seq.to_le_bytes());
    hasher.update(&[state]);
    hasher.update(padding);
    hasher.finalize()
}

fn read_otadata_entries(
    flash: &mut esp_storage::FlashStorage,
) -> [OtaSelectEntry; 2] {
    let mut buf = [0u8; 24]; // 2 entries × 12 bytes
    match flash.read(OTADATA_OFFSET, &mut buf) {
        Ok(()) => {}
        Err(_) => {
            println!("otadata read failed, returning invalid entries");
            return [
                OtaSelectEntry {
                    ota_seq: 0xFFFFFFFF,
                    ota_state: OTA_STATE_UNUSED,
                    padding: [0, 0, 0],
                    crc: 0,
                },
                OtaSelectEntry {
                    ota_seq: 0xFFFFFFFF,
                    ota_state: OTA_STATE_UNUSED,
                    padding: [0, 0, 0],
                    crc: 0,
                },
            ];
        }
    }
    let entry_size = core::mem::size_of::<OtaSelectEntry>();
    let entry0 =
        unsafe { core::ptr::read(buf[0..entry_size].as_ptr() as *const OtaSelectEntry) };
    let entry1 = unsafe {
        core::ptr::read(buf[entry_size..entry_size * 2].as_ptr() as *const OtaSelectEntry)
    };
    [entry0, entry1]
}

fn set_otadata_to_ota0(flash: &mut esp_storage::FlashStorage) {
    let entries = read_otadata_entries(flash);

    // Validate CRCs and find highest valid seq
    let mut max_seq: u32 = 0;
    for entry in &entries {
        let expected_crc =
            calc_otadata_crc(entry.ota_seq, entry.ota_state, &entry.padding);
        if expected_crc == entry.crc && entry.ota_seq > max_seq {
            max_seq = entry.ota_seq;
        }
    }

    let new_seq = max_seq.wrapping_add(1);

    // Build new entry pointing to ota_0
    let new_entry = OtaSelectEntry {
        ota_seq: new_seq,
        ota_state: OTA_STATE_VALID,
        padding: [0, 0, 0],
        crc: calc_otadata_crc(new_seq, OTA_STATE_VALID, &[0, 0, 0]),
    };

    // Build buffer for the full otadata partition
    let mut buf = [0xFFu8; OTADATA_SIZE as usize];
    let entry_size = core::mem::size_of::<OtaSelectEntry>();
    let entry_bytes = unsafe {
        core::slice::from_raw_parts(
            &new_entry as *const OtaSelectEntry as *const u8,
            entry_size,
        )
    };
    buf[0..entry_size].copy_from_slice(entry_bytes);

    // Erase and write otadata sector
    flash.erase(OTADATA_OFFSET, OTADATA_SIZE).unwrap();
    flash.write(OTADATA_OFFSET, &buf).unwrap();

    println!(
        "otadata updated -> ota_0 (seq={}, crc=0x{:08X})",
        new_seq, new_entry.crc
    );
}

// ──────────────────────────────────────────────────────────────
// Check if ota_0 has a valid ESP32 image header
// ──────────────────────────────────────────────────────────────

fn check_ota0_valid(flash: &mut esp_storage::FlashStorage) -> bool {
    let mut magic = [0u8; 1];
    match flash.read(OTA0_OFFSET, &mut magic) {
        Ok(()) => magic[0] == ESP_IMAGE_MAGIC,
        Err(_) => false,
    }
}

// ──────────────────────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────────────────────

#[entry]
fn main() -> ! {
    let peripherals = Peripherals::take();
    let system = SystemControl::new(peripherals.SYSTEM);
    let clocks = ClockControl::max(system.clock_control).freeze();
    let delay = Delay::new(&clocks);

    // Initialize GPIO9 (BOOT button) with internal pull-up
    let io = Io::new(peripherals.GPIO, peripherals.IO_MUX);
    let mut boot_btn = Input::new(io.pins.gpio9, Pull::Up);

    // Initialize SPI flash
    let spi = esp_hal::spi::master::Spi::new(
        peripherals.SPI2,
        esp_hal::spi::SpiMode::Mode0,
        &clocks,
    )
    .unwrap()
    .with_sck(io.pins.gpio6)
    .with_mosi(io.pins.gpio7)
    .with_miso(io.pins.gpio2)
    .with_cs(io.pins.gpio10);

    let mut flash = esp_storage::FlashStorage::new(spi);

    println!("=== ESP32-C3 Bare-Metal BLE Recovery ===");
    println!("Factory partition @ 0x{:08X}", FACTORY_OFFSET);
    println!("OTA0 partition    @ 0x{:08X}", OTA0_OFFSET);
    println!("otadata partition @ 0x{:08X}", OTADATA_OFFSET);

    // ─── Boot Decision Logic ───
    let button_pressed = boot_btn.is_low();
    let ota0_valid = check_ota0_valid(&mut flash);

    println!("BOOT button (GPIO9) pressed: {}", button_pressed);
    println!("ota_0 image valid (0xE9 magic): {}", ota0_valid);

    if !button_pressed && ota0_valid {
        // Normal boot path: ensure otadata points to ota_0, then reset
        println!("Normal boot: switching to ota_0...");
        set_otadata_to_ota0(&mut flash);
        delay.delay_millis(100);
        software_reset();
    }

    // ─── Recovery Mode ───
    println!("RECOVERY MODE: BLE OTA server starting...");
    println!("  - Button pressed OR ota_0 invalid -> staying in recovery");

    // TODO: Initialize BLE GATT server (bleps + esp-wifi)
    // The device stays here in a loop until power-cycled or new firmware uploaded.
    println!("Recovery mode active. Awaiting BLE OTA upload...");
    loop {
        delay.delay_millis(500);
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    esp_println::println!("PANIC: {}", info);
    loop {}
}