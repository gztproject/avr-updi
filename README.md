# AVR UPDI

Program AVR microcontrollers over UPDI from a Flipper Zero. The link is UPDI, not ISP.

Currently supported: AVR128DA28, AVR128DA32, AVR128DA48, and AVR128DA64 (including the S variants). These parts use NVM controller P:2: 24-bit addresses, 512-byte flash pages, and word writes.

## Wiring

Flipper GPIO is 3.3 V. Power the target at 3.3 V. Pin 9 can supply it. Do not connect a 5 V target, and do not try to put 12 V on a GPIO pin. If the UPDI pin has been fused to GPIO, only a high-voltage UPDI programmer can recover the part.

The default pair is pins 15 and 16. Open **Pins** in the app and press left or right to use pins 13 and 14 instead. The choice is saved on the SD card.

| Flipper | Target |
| --- | --- |
| 15 TX, through 1 kΩ (or 13 TX) | UPDI |
| 16 RX, direct (or 14 RX) | same UPDI node |
| GND | GND |
| 9 3V3, optional | VCC |

The resistor lets the target override TX so the Flipper can discard the byte it just sent. The Wiring screen shows the pair that is selected.

Close the USB-UART bridge before opening the app. The app takes the selected UART and turns expansion-module detection off while it runs.

## What it does

- Read the signature and show the part name
- Chip-erase, flash an Intel HEX file from the SD card, and verify it
- Dump all 128 KB of flash to `/ext/apps_data/avr_updi/flash.hex`
- Read the fuse bytes

Chip erase clears flash. It also clears EEPROM unless the EESAVE fuse is set. Fuse and EEPROM writes are not in this version. A locked chip is erased with the NVMErase key before it can be programmed.

HEX files are read one 512-byte page at a time, including records above 64 KB. Addresses in the file must increase.

The link starts at 115200 8E2. After the part answers, the app asks for an 8 MHz UPDI clock and 230400 baud, and falls back to 115200 if that does not answer.

## Build

Install [ufbt](https://github.com/flipperdevices/flipperzero-ufbt), then from this directory:

```sh
ufbt
```

Copy `dist/avr_updi.fap` to `apps/GPIO/` on the SD card.

The Intel HEX assembler has a host test:

```sh
cc -std=c11 -Wall -Wextra -Werror -I. tests/test_intel_hex.c hex/intel_hex.c -o tests/test_intel_hex
./tests/test_intel_hex
```

On-device checks need a real AVR128DA: the signature matches, a disconnected UPDI line times out, a HEX that lives above 64 KB verifies, and a dump round-trips.
