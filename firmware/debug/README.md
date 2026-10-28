# Hardware Debug Console

This is the command reference for [`debug.ino`](debug.ino), a standalone Arduino sketch used to bring up and inspect the peripherals on the custom ATmega328P-AU PCB. It exercises the hardware interfaces **without** running the heart-rate or SpO₂ application. For the relationship between this sketch and the main firmware, see the [Firmware Overview](../../docs/firmware-overview.md).

The commands below describe what the firmware does and what can be checked on the physical board. A successful serial response alone is not always proof that the connected hardware works correctly; several checks also require observing the buttons, displays, or sensor readings.

## 1. Setup and Command Format

1. Open `debug/debug.ino` in Arduino IDE, keeping the complete `debug/` sketch folder and its header files.
2. Select an **ATmega328P, 16 MHz** Arduino AVR target and the board/processor/upload settings compatible with the Bootloader already programmed on this PCB.
3. Upload through the board's Mini USB / CH340C serial interface. Uploading this sketch replaces the main application until it is uploaded again.
4. Open Serial Monitor at **115200 baud**, with **Newline** or **Both NL & CR** selected, and send `help`.

Commands are case-sensitive and accept one command per line. Decimal numbers are used by default; prefix a value with `0x` for hexadecimal. The input buffer accepts up to **79 characters** per line. CR, LF and CRLF are supported; an overlong line is discarded rather than executed partially.

At startup, key-event reporting is enabled, but the seven-segment display is blank, MAX30102 sampling is stopped, and the OLED is not initialised. **S1 is the hardware RESET button**, not a key reported by this console.

## 2. Suggested Board Bring-Up Sequence

The following sequence separates basic electrical communication from observable peripheral operation:

| Step | Commands | What to check |
| --- | --- | --- |
| Buttons | `keys on`, `keys read` | Press and release S2–S4; check raw input levels and `PRESS` / `RELEASE` / `CLICK` messages. |
| Seven-segment display | `seg test`, then `seg digits 0123` | Inspect all four digit positions and the seven segment lines. |
| I²C bus | `i2c scan` | Look for ACK responses at **0x3C** (OLED) and **0x57** (MAX30102). |
| MAX30102 | `max id`, `max begin`, `max stream on 100` | Confirm register communication, then observe red/IR values while placing and removing a finger. |
| OLED | `oled begin`, wait for `OLED job done`, then `oled test` | Inspect the full border, corner labels and two lines of identification text on the screen. |

These steps can be performed separately. The OLED and MAX30102 are connected to the same I²C bus, but each has its own initialisation and test commands.

## 3. Complete Command Reference

### 3.1 General Status and Buttons

| Command | Behaviour and diagnostic purpose |
| --- | --- |
| `help` | Prints the command list supported by the running debug firmware. |
| `status` | Reports firmware uptime, key-event queue drops, segment output status, MAX30102 sampling state, and OLED software state. These are internal status reports, not independent measurements of hardware health. |
| `pins` | Prints the configured MCU/Arduino pin mapping. Use it to compare `BoardConfig.h` with the PCB schematic; it does not electrically verify continuity. |
| `keys on` / `keys off` | Enables or suppresses printed button events. GPIO polling and debouncing continue even when printing is off. Event reporting is on by default. |
| `keys read` | Prints each S2–S4 button's immediate GPIO level (`raw`) and debounced `pressed` state. Released inputs should be HIGH and pressed inputs LOW because the PCB has external pull-ups. |

The key driver recognises `PRESS`, `RELEASE`, `CLICK` and `LONG` events. It uses approximately **25 ms debounce** and an **800 ms long-press threshold**. These events let the user check that mechanical switch transitions are interpreted consistently, beyond merely observing a raw voltage level.

### 3.2 Four-Digit Seven-Segment Display

| Command | Behaviour and diagnostic purpose |
| --- | --- |
| `seg number <n>` | Displays an integer from **-999 to 9999**, suppressing unnecessary leading zeros. Example: `seg number -123`. |
| `seg digits <text>` | Displays exactly four characters, including leading zeros. Digits `0`–`9`, `-` (dash) and `_` (blank) are supported. Example: `seg digits 1-2_`. |
| `seg raw <digit> <mask>` | Lights selected segments on only one digit (**1–4**). The mask is **0–0x7F**, with bits 0–6 representing segments a–g. Examples: `seg raw 1 0x01` lights segment a on DIG1; `seg raw 4 0x40` lights segment g on DIG4. |
| `seg test` | Walks through segments a–g on DIG1–DIG4, approximately **seven seconds** total, then displays `1234`. This is the primary visual test for segment and digit-select wiring. |
| `seg clear` | Clears the four-digit display buffer. |
| `seg off` / `seg on` | Disables or restores display output while preserving the current buffer. Either command cancels a running `seg test`. |

The display has no wired decimal-point segment. Its common-cathode digit selects are active-low, while the segment lines are active-high. A **Timer2 interrupt** scans the four digits at a 1 kHz slot rate (250 Hz per digit). Visual tests can reveal missing or swapped segments, but do not establish electrical current margins or long-term reliability.

### 3.3 Shared I²C Bus

| Command | Behaviour and diagnostic purpose |
| --- | --- |
| `i2c scan` | Scans addresses **0x01–0x7E**, prints responding ACK addresses, and reports a device count. The expected module addresses are **0x3C** (OLED) and **0x57** (MAX30102). |

An ACK indicates that something responds at an address; it does not identify the device conclusively or prove that display drawing or sensor acquisition works. Use the specific commands below for those checks.

### 3.4 MAX30102 Sensor

| Command | Behaviour and diagnostic purpose |
| --- | --- |
| `max id` | Reads and prints `PART_ID` and `REV_ID` from address 0x57. This verifies register-read communication. The expected compatible part ID is `0x15`, but related MAX3010x devices may share that ID. |
| `max begin` | Resets and configures the sensor for red/IR acquisition at **100 samples/s**, **18-bit** data and no hardware averaging; clears its FIFO and starts sampling. Default red/IR LED register codes are both **31**. |
| `max read` | Prints the latest sample already received by the MCU, including red/IR values, received-sample count, reception time and sample age. It does not wait for a fresh sample. |
| `max stream on` / `max stream on <ms>` | Periodically prints the **latest** red/IR sample. The default interval is **100 ms**; allowed values are **20–5000 ms**. Background FIFO acquisition continues independently of the printing interval. |
| `max stream off` | Stops periodic serial output **without stopping sensor acquisition**. |
| `max led <red> <ir>` | Changes the two LED register codes (**0–255** each) after `max begin`. These codes are not direct milliampere values; changing them can help inspect ADC response. |
| `max stats` | Reports whether the sensor is running, how many samples have been received, the minimum detected FIFO-loss count (`lost_min`), and the current stream interval. |
| `max stop` | Stops serial streaming and places the sensor in shutdown. The last cached sample remains available for inspection. |

For example, after `max begin`, use `max stream on 100` and compare readings with and without a finger over the sensor. A changing ADC value supports the conclusion that optical sampling is active; it **does not verify heart-rate or SpO₂ accuracy**. The serial stream is not a lossless dump of every 100 Hz sample. `lost_min` is a lower bound because the device's FIFO overflow counter can saturate.

### 3.5 SSD1306 OLED

| Command | Behaviour and diagnostic purpose |
| --- | --- |
| `oled status` | Prints the configured display profile, drawing address, software state, busy/ready flags, on/invert/flip settings, and most recent probe result. |
| `oled probe 0x3C` / `oled probe 0x3D` | Checks for an I²C ACK at either address, without changing the active drawing address or identifying the controller. A failed alternate-address probe does not replace the configured 0x3C address. |
| `oled begin` / `oled begin 0x3C` | Initialises the board's OLED at its fixed **0x3C** address. `oled begin 0x3D` is rejected for this board. Wait for `OLED job done` before further drawing commands. |
| `oled test` | Restores normal polarity and reference orientation, then draws a full border, TL/TR/BL/BR corner labels, and `SSD1306 128x32` / `0x3C WHITE OLED`. A visual check tests more than address ACK alone. |
| `oled pattern border` | Draws a one-pixel border around the 128×32 display area to inspect the edges. |
| `oled pattern checker` | Draws an 8×8 checkerboard pattern to inspect addressing and the display area. |
| `oled clear` | Writes zeros across the entire display RAM; it does not switch the panel off. |
| `oled text <row> <text>` | Writes ASCII text to row **0–3** (up to **21 printable characters**). Example: `oled text 1 HELLO OLED`. Spaces are allowed; the chosen row is rewritten through its right edge. |
| `oled fill 0` / `oled fill 1` | Fills display RAM with all-zero or all-one pixels. Inversion affects how these values appear on screen. |
| `oled invert 0` / `oled invert 1` | Selects normal or inverted pixel polarity without rewriting the underlying image. |
| `oled flip 0` / `oled flip 1` | Selects the reference orientation or rotates the image by 180°. |
| `oled contrast <value>` | Writes an SSD1306 contrast setting from **0–255**. This is a controller setting, not a calibrated brightness measurement. |
| `oled off` / `oled on` | Turns the panel display off/on while retaining its display RAM contents. |

OLED initialisation, clearing, text and pattern drawing run as **queued jobs**. The driver sends at most **16 bytes of pixel data per update** and does not allocate a full-screen framebuffer. Wait for `OLED job done` after a job before issuing another drawing command; a command sent while the driver is busy may report `ERR OLED code=3`. Text is limited to the included ASCII font; this firmware has no Chinese-character font.

## 4. Interpreting Errors and Results

- `ERR arguments`: wrong argument count, format or numeric range.
- `ERR line too long`: the entire overlong input line was discarded.
- I²C address/data **NACK** or a timeout: inspect the relevant module connection, supply and bus conditions. A bus-status error should not be treated as a definitive diagnosis of a single physical cause.
- `ERR MAX`: inspect the sensor error code and `last_bus_error`. If a running session fails, it may be necessary to repeat `max begin`.
- `ERR OLED code=2`: initialise the display first; **code 3**: wait for the current job; a bus fault may require another `oled begin`.

The checks above were designed for the assembled prototype and its particular modules. Earlier bring-up established basic button, display and MAX30102 communication behaviour, but these commands alone do **not** validate the current main application's physiological estimates, electrical margins, or long-term stability. Refer to the [Hardware Design Notes](../../docs/design-notes.md) for the circuit and its known limitations.
