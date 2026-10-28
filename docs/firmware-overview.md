# Firmware Overview

This document describes the firmware developed for the custom ATmega328P-AU board. It focuses on the connection between the schematic and the code, the use of a separate hardware-debugging program, and the behaviour of the main application. The goal is to demonstrate practical **board bring-up, signal processing and embedded firmware integration**, rather than a general-purpose software framework.

The underlying circuit is covered in [Hardware Design Notes](design-notes.md). Initial ISP Bootloader programming is documented separately in [Bootloader Programming Setup](programming-setup.md).

## 1. Firmware Structure

The firmware contains two **independent Arduino sketches**:

| Sketch | Purpose |
| --- | --- |
| [`firmware/debug/debug.ino`](../firmware/debug/debug.ino) | An interactive serial console for checking the board's GPIO, displays, I²C bus and MAX30102 sensor individually. |
| [`firmware/main/main.ino`](../firmware/main/main.ino) | The main application, combining the peripheral drivers with optical data processing, display updates and button-controlled interaction. |

Each sketch has its own copies of the required header files and can be opened and uploaded independently in Arduino IDE. The two sketches are **alternatives**, not two programs that run simultaneously on the MCU. Their shared driver implementations are currently kept in both folders to make each sketch self-contained.

Both target an **ATmega328P at 16 MHz**, using the Arduino AVR core and its bundled `Wire` library. The source does not require installing a separate MAX30102 or OLED library. Third-party contributions incorporated into the source, including OLED font data and part of the optical estimation method, are identified in [THIRD_PARTY_LICENSES.txt](../firmware/THIRD_PARTY_LICENSES.txt).

## 2. Hardware Interfaces and Pin Mapping

The mapping in `BoardConfig.h` follows the custom PCB schematic rather than the physical layout of an Arduino Uno development board.

| Board function | ATmega328P signal | Arduino pin or interface | Firmware behaviour |
| --- | --- | --- | --- |
| S2 / S3 / S4 buttons | PC1 / PC2 / PC3 | A1 / A2 / A3 | Active-low inputs with external pull-ups and software debouncing. |
| Seven-segment lines a–g | PD2–PD7, PB0 | D2–D8 | Segment outputs; HIGH turns a segment on. |
| Digit selects DIG1–DIG4 | PB2–PB5 | D10–D13 | Active-low selection for four-digit multiplexing. |
| MAX30102 and OLED SDA / SCL | PC4 / PC5 | A4 / A5 (`Wire`) | Shared 100 kHz I²C bus. |
| CH340C serial connection | PD0 / PD1 | D0 (RX) / D1 (TX) | Sketch upload through the Bootloader; interactive serial commands are provided by the separate debug sketch. |
| S1 reset button | PC6 / RESET | RESET | Hardware reset; not an application input. |

The MAX30102 uses I²C address **0x57**; the 128×32 SSD1306 OLED uses **0x3C** on this board. The seven-segment display is driven directly by GPIO and scanned using **Timer2**: a 1 kHz scan schedule gives each digit a 250 Hz refresh rate. Timer2 is therefore reserved for the display and should not be reassigned to another timing library without revisiting the driver.

Pins D10–D13 also share the MCU's SPI/ISP signals. The complete electrical connections can be checked against the [schematic PDF](../hardware/schematic/schematic.pdf).

## 3. Hardware Debug Console

The [debug sketch](../firmware/debug/debug.ino) is intended for **peripheral bring-up before application-level integration**. Its serial console isolates individual hardware interfaces from the main application's signal-processing logic. After uploading it, open Arduino IDE Serial Monitor at **115200 baud**, select **Newline** or **Both NL & CR**, and send `help`.

The most useful commands form a progression from basic communication to observable peripheral behaviour:

| Peripheral | Key commands | What they establish |
| --- | --- | --- |
| **Buttons** | `keys read` and `keys on` | Physical S2–S4 presses change GPIO levels and produce debounced input events. |
| **Seven-segment display** | `seg raw 1 0x01`, `seg test` | Individual segment/digit selection and the complete four-digit scan can be inspected visually. |
| **I²C bus** | `i2c scan` | Devices respond at the expected addresses, **0x3C** and **0x57**; ACK alone does not prove full functionality. |
| **MAX30102** | `max id`, `max begin`, `max stream on 100` | Register reads and sensor initialisation work, and real red/IR ADC values can be inspected for changes. |
| **OLED** | `oled begin`, then `oled test` | The panel can be initialised and its borders, orientation and text output checked on the actual screen. |

The debug sketch does not start MAX30102 sampling or initialise the OLED automatically. OLED drawing is performed as a queued job; wait for `OLED job done` after `oled begin` before sending `oled test`. The checks above do not involve heart-rate or SpO₂ estimation.

For **all supported commands**, argument ranges, output interpretation and basic error handling, see the separate [Hardware Debug Console command reference](../firmware/debug/README.md). This keeps the firmware overview focused on the board-level verification process while leaving the interactive tool documented alongside its source.

During earlier board bring-up, the seven-segment ordering, button events, OLED operation at address 0x3C, and basic MAX30102 communication/raw readings were checked on the physical prototype. These observations support the hardware-interface work, but they are not a complete validation of the later main application.

## 4. Main Application

### 4.1 Interaction and Display Modes

The [main sketch](../firmware/main/main.ino) starts in **STATS** mode and automatically initialises the MAX30102 and OLED. It continuously acquires optical samples and manages two display modes:

| Mode | OLED | Four-digit display |
| --- | --- | --- |
| **RAW** | Latest full-resolution red and infrared ADC readings. | Briefly flashes `----` for approximately 100 ms when a pulse is detected. |
| **STATS** (default) | Heart-rate/status information and an uncalibrated `SpO2 EST` value when available. | Estimated heart rate in BPM; otherwise a placeholder. |

The four board buttons have distinct roles:

- **S1:** Reset the MCU and restart the application in its default state.
- **S2:** Restart the current measurement while retaining the selected RAW/STATS view.
- **S3:** Switch to RAW view without discarding the measurement history.
- **S4:** Switch to STATS view without discarding the measurement history.

The application separates the **display mode** (`Raw` or `Stats`) from the **measurement phase** (`Waiting`, `Collecting`, `Measuring` or `Fault`). This allows the user to change views while sampling continues. When a finger is removed or a result is invalid, the corresponding value is withheld rather than replaced with a plausible-looking number. Communication failures can place the application in a fault state; S2 requests reinitialisation.

### 4.2 From Optical Samples to Displayed Values

The MAX30102 is configured to provide paired **18-bit red and infrared (IR) ADC samples at 100 samples per second**. In RAW mode, the OLED shows these latest ADC counts directly; they are not heart-rate or oxygen-saturation values. In STATS mode, `Measurement.h` processes the samples as follows.

**Finger detection.** The IR channel is used to decide whether a finger is present. By default, 20 consecutive samples at or above an IR threshold of **5000** are required to enter measurement, while 30 consecutive samples below **3500** indicate removal. This hysteresis reduces repeated state changes around a single threshold; the defaults are starting values, not thresholds calibrated for every physical setup.

**Heart rate (BPM).** The streaming IR detector tracks and removes the slowly varying DC baseline, then subtracts a slower trend before smoothing the remaining pulsatile component. These inexpensive filter stages help separate heartbeat-related variation from gradual drift in the optical signal. A fixed-point (Q8) amplitude envelope supplies an adaptive detection threshold. The detector tracks local valleys and identifies candidate peaks from their **prominence above the preceding valley**.

Candidate peak intervals are measured in raw 100 Hz sample ticks. Very short secondary peaks are ignored without moving the trusted timing anchor. Up to four intervals are retained; a median-centred average reduces the influence of isolated outliers. At least three mutually consistent intervals are required to establish a valid heart rate. The firmware also checks how recently a trustworthy interval was observed, and can adopt a substantially faster rhythm when three consecutive candidate intervals support the new cadence. To reduce abrupt displayed changes, the accepted period is smoothed before conversion to BPM:

$$
\mathrm{BPM} \approx \frac{60f_s}{\widehat{N}} = \frac{6000}{\widehat{N}}, \qquad f_s = 100\ \mathrm{Hz}
$$

Here \(\widehat{N}\) is the **smoothed estimate of the accepted inter-peak interval**, in 100 Hz sample ticks. Existing valid estimates are updated by moving approximately one-quarter of the way towards the newly accepted period; a newly established rhythm starts from its current period. Heart-rate validity is based on consistent, recent peak timing. The separate four-second IR periodicity check described below is used when accepting an SpO₂ estimate, **not as a prerequisite for displaying an otherwise valid BPM**. BPM appears on the four-digit display in STATS mode and as text on the OLED.

**SpO₂ engineering estimate.** Every four red/IR sample pairs are averaged into a compact **25 Hz** record. The firmware retains **100 16-bit records per channel**, representing a rolling **four-second window** in **400 bytes** of sample storage. For each channel, it computes the mean (DC component), removes the mean and linear trend, then finds the root-mean-square amplitude of the remaining AC component. Their normalised ratio is:

$$
R = \frac{\mathrm{RMS}(AC_{red})/DC_{red}}{\mathrm{RMS}(AC_{IR})/DC_{IR}}
$$

The firmware then evaluates the reference polynomial:

$$
\mathrm{SpO}_{2,\mathrm{EST}}(\%) = -45.060R^2 + 30.354R + 94.845
$$

The estimate is updated from the rolling window approximately once per second. With one accepted estimate the value is shown directly; with two it uses their mean, and with three it uses their median to reduce isolated changes. The code also checks pulse strength, excessive optical variation, saturation, red/IR correlation, IR periodicity, abrupt changes and the permitted ratio/output ranges. Estimates outside **70–100%** are rejected rather than clamped into that range. If an estimate fails these checks, the display shows `SpO2 EST: --%` instead of an arbitrary percentage. Heart rate and SpO₂ have **separate validity flags**, so a usable BPM does not automatically imply a usable SpO₂ estimate.

### 4.3 Implementation Choices and Attribution

The implementation also uses fixed-size buffers, non-blocking display scheduling and a small OLED text renderer instead of a full-screen framebuffer. These are practical choices for the ATmega328P's limited RAM. The main sketch is operated through the physical buttons and displays and does not use a runtime serial console. The independently uploadable **debug sketch** provides interactive serial commands for peripheral inspection.

The project-specific streaming peak detector, compact optical window and application state management are implemented in [`Measurement.h`](../firmware/main/Measurement.h). The **detrended RMS red/IR ratio approach and SpO₂ reference polynomial** were adapted from Robert Fraczkiewicz's [MAX30102_by_RF algorithm](https://github.com/aromring/MAX30102_by_RF/blob/master/algorithm_by_RF.cpp), which is distributed under the **MIT licence**. This is not a full port of that algorithm; in particular, the project's BPM calculation uses its own streaming peak-timing approach. The OLED's ASCII font subset was adapted from [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library/blob/master/glcdfont.c) under a **BSD licence**. The relevant notices and source details are retained in [`THIRD_PARTY_LICENSES.txt`](../firmware/THIRD_PARTY_LICENSES.txt).

**The reference polynomial is not a calibration of this board.** Its coefficients were not fitted against reference measurements for the actual MAX30102 module and optical arrangement. Analog Devices also notes that SpO₂ mapping depends on the optical design and requires calibration; see its [SpO₂ measurement guidelines](https://www.analog.com/en/resources/technical-articles/guidelines-for-spo2-measurement--maxim-integrated.html). Therefore `SpO2 EST` is a demonstration of signal processing, **not a validated physiological reading**.

For interactive hardware diagnostics, upload the separate [debug sketch](../firmware/debug/debug.ino) and follow its [command reference](../firmware/debug/README.md). The debug and main sketches do not run simultaneously.

## 5. Build and Upload

With a compatible Bootloader already installed, the intended firmware upload path is **Mini USB → CH340C → ATmega328P UART**. This is separate from the six-pin ISP connection used for initial Bootloader programming.

1. Keep the complete `firmware/main/` or `firmware/debug/` folder together with its header files.
2. Open the matching `main.ino` or `debug.ino` in Arduino IDE.
3. Select an **ATmega328P, 16 MHz** Arduino AVR target and the board/processor settings appropriate for the Bootloader already present on the MCU. Select the serial port exposed through the CH340C.
4. Compile and upload the selected sketch. Only one of the two sketches will reside in the MCU application area at a time.
5. When running the **debug sketch**, open its serial console at **115200 baud** with a newline-compatible line ending. The main sketch uses the board buttons and displays instead of serial commands. Opening the serial port may trigger the board's DTR auto-reset circuit.

The exact original Bootloader and fuse configuration were not recorded, so this note does not prescribe a universal Uno/Nano processor-menu choice or require burning a new Bootloader. Refer to [programming-setup.md](programming-setup.md) for the documented ISP procedure.

## 6. Validation Boundaries

This is a **prototype firmware implementation**, not a medically validated measurement instrument. In particular:

- The `SpO2 EST` output is an **uncalibrated engineering estimate**. Its numeric value must not be treated as an accurate blood-oxygen measurement or used for health decisions.
- On-board tests established basic peripheral operation and exercised the integrated main application, including optical signal recognition and display behaviour. These observations support functional integration but do not establish quantified measurement accuracy or reliability.
- The detector includes measures to reject noisy or inconsistent pulse candidates, but sustained operation, behaviour across users and optical conditions, and agreement with a reference instrument have not been systematically characterised.
- The current display, GPIO and signal-processing choices are specific to this PCB. They are not presented as a portable driver framework or a production-ready design.

The firmware's role in this repository is to document the progression from **hardware bring-up** to an integrated embedded application, while keeping those verification limits explicit.
