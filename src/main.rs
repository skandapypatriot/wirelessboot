#![no_std]
#![no_main]

use esp_hal::{
    clock::ClockControl,
    delay::Delay,
    gpio::{Input, Io, Pull},
    peripherals::Peripherals,
    prelude::*,
    reset::software_reset,
    system::SystemControl,
};

// ──────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────

const OTA0_OFFSET: u32 = 0x10000;
const BOOT_BUTTON_GPIO: u8 = 9;
const ESP_IMAGE_MAGIC: u8 = 0xE9;

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

    // Read first byte of ota_0 to check for valid ESP32 image
    // For now, just check the button
    let button_pressed = boot_btn.is_low();

    if !button_pressed {
        // Normal boot: software reset to let bootloader load ota_0
        software_reset();
    }

    // ─── Recovery Mode ───
    loop {
        delay.delay_millis(500);
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}