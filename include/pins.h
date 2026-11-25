// ESP32-2432S028R "Cheap Yellow Display" pins, plus where the LD2410B radar plugs in.
#pragma once

namespace Pin {
// Radar (LD2410B) on the CN1 connector: radar TX -> IO27, radar RX <- IO22. Power from P1 (VIN = 5 V).
constexpr int RadarRx = 27;  // ESP32 receives here (radar TX)
constexpr int RadarTx = 22;  // ESP32 sends here (radar RX)

// Display: ILI9341 on the HSPI bus
constexpr int TftSck = 14, TftMiso = 12, TftMosi = 13, TftCs = 15, TftDc = 2, TftBacklight = 21;
// Touch: XPT2046, bit-banged (the SD card needs the second hardware SPI bus)
constexpr int TouchClk = 25, TouchMiso = 39, TouchMosi = 32, TouchCs = 33;
// SD card slot on the VSPI bus
constexpr int SdSck = 18, SdMiso = 19, SdMosi = 23, SdCs = 5;
// RGB LED on the back (active LOW), speaker amplifier ("SPEAK" connector), BOOT button
constexpr int LedRed = 4, LedGreen = 16, LedBlue = 17;
constexpr int Speaker = 26;
constexpr int BootButton = 0;
}  // namespace Pin
