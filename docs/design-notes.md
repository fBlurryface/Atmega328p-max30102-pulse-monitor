# Hardware Design Notes

This document describes the hardware design of a custom ATmega328P-AU board developed as an embedded-systems prototype. Its scope is deliberately practical: the basic circuit principles, the interfaces required by the application, and how the schematic was translated into a manufactured PCB. Firmware architecture, MCU pin definitions in code, and the Bootloader programming procedure are reserved for separate documentation.

The board is an **initial prototype**, not a production-qualified design. The schematic and PCB files record what was built; the limitations noted below are opportunities for a possible future revision, not claims that the layout was fully optimised.

## 1. Hardware Overview

The board integrates an 8-bit AVR microcontroller with a MAX30102 optical sensor module, a four-pin I²C OLED module, a four-digit seven-segment display, user buttons, and on-board USB-to-UART communication. A 6-pin ISP interface allows direct programming of the microcontroller independently of its serial Bootloader.

The ATmega328P-AU coordinates the peripherals. The MAX30102 and OLED share an I²C bus; the seven-segment display and buttons use GPIO. A Mini USB connector provides the board's 5 V supply and a serial communication path through the CH340C.

The design consists of a single schematic sheet and a routed PCB created in Altium Designer. See the [schematic PDF](../hardware/schematic/schematic.pdf), [PCB layout PDF](../hardware/pcb/pcb-layout.pdf), and [BOM](../hardware/bom.csv) for the underlying design files and component values.

## 2. Schematic Design

### 2.1 MCU Minimum System: Power, Clock and Reset

The ATmega328P-AU is powered from the board's **5 V rail**, which is supplied through Mini USB. Both the digital supply and AVCC are connected to this rail. The supply network includes a **10 µF capacitor (C1)** for local energy storage and a **100 nF capacitor (C3)** for high-frequency bypassing. The AREF pin is bypassed to ground through **C9 (100 nF)**.

An external **16 MHz crystal (Y1)** is connected to the MCU oscillator pins. Two **22 pF load capacitors (C2 and C5)** connect the crystal terminals to ground; **R2 (1 MΩ)** is connected across the oscillator terminals. This circuit provides the external clock source intended for the AVR configuration.

The RESET line is normally pulled high by **R1 (10 kΩ)** and can be pulled low with the dedicated **S1** pushbutton. It also connects to the USB-to-UART circuit through **C4 (100 nF)**, allowing a transition on DTR to generate a reset pulse for serial Bootloader uploads.

These components form the essential support circuitry for a stand-alone AVR rather than relying on a preassembled Arduino development board.

### 2.2 Programming and Communication Interfaces

The PCB provides two complementary programming paths:

- **ISP:** A standard 2×3 connection exposes **MISO, MOSI, SCK, RESET, VCC and GND**. An external AVR ISP programmer can access the MCU directly, including for initial Bootloader and fuse configuration. The BOM marks the permanent header as *DNP*; pins were intended to be inserted temporarily for programming.
- **USB-to-UART:** The **CH340C** connects the Mini USB data lines to the MCU's UART RX/TX signals, with **R3 and R4 (1 kΩ)** in series on those lines. Its DTR signal is AC-coupled to RESET through C4. With a suitable Bootloader already programmed, this path can be used for normal serial firmware uploads.

The CH340C provides a USB serial interface but does **not** itself program a blank ATmega328P through UART. The external ISP programmer and the Bootloader serve different purposes. Actual programmer settings and the initialisation procedure belong in [`programming-setup.md`](programming-setup.md).

### 2.3 Sensor and OLED Interface

The **MAX30102 sensor module** and **0.91-inch OLED module** share the same I²C **SDA** and **SCL** nets from the MCU. Using a common bus limits the number of GPIO signals needed for these two peripherals.

Both module connectors are connected to the board's 5 V and ground nets in the schematic. Because these are **modules** rather than bare MAX30102 and OLED controller ICs, their on-board regulation, I²C pull-ups, and logic-level compatibility must be checked against the particular assembled modules. The main-board schematic does not arrange separate I²C pull-up resistors.

### 2.4 Display and User Inputs

The four-digit **common-cathode seven-segment display** has seven segment lines, each driven through a **1 kΩ resistor (R8–R14)**. The four digit-select lines are connected directly to MCU GPIO. The intended firmware method is time-multiplexed scanning: select one digit at a time while driving the required segments. As there is no separate display-driver IC or digit transistor stage, segment brightness and GPIO current limits are practical constraints to consider during firmware development.

Three functional pushbuttons (**S2–S4**) are connected as active-low inputs. Each input has an external **10 kΩ pull-up resistor (R5–R7)** to 5 V and is connected to ground when pressed. **S1** is reserved for hardware reset rather than application input.

## 3. PCB Implementation

### 3.1 Physical Arrangement

The PCB places the MCU near the central area of the board, with the USB connector at an edge. The MAX30102 module, OLED module, seven-segment display, and tactile switches occupy their respective areas of the board. This arrangement brings the electrical interfaces and user-facing components together on one custom PCB.

### 3.2 Routing and Physical Prototype

The layout uses copper routing on the top and bottom sides to connect the MCU, support components, modules, and connectors. The exported [PCB layout](../hardware/pcb/pcb-layout.pdf) shows the actual placement and routing, rather than an idealised diagram.

The design was subsequently fabricated and populated. The [bare PCB](../images/pcb-bare.jpg) and [assembled PCB](../images/pcb-assembled.jpg) photographs provide direct evidence of the transition from Altium design files to a physical prototype. This is evidence of manufacture and assembly, not by itself a comprehensive electrical or functional validation.

## 4. Limitations and Potential Improvements

The first PCB revision met the immediate goal of producing an assembled custom board, but its layout was not systematically optimised:

- **MCU decoupling placement:** The 100 nF supply bypass capacitor could be positioned closer to the relevant MCU power and ground pins, with shorter connections.
- **Oscillator placement:** The crystal and its two 22 pF **load capacitors** could be arranged more tightly around the MCU oscillator pins to shorten the oscillator traces and loop.
- **Routing around the MCU:** Some connections are relatively dense and indirect. A revised component orientation and more deliberate escape routing could improve clarity and reduce unnecessary trace length.

A future PCB revision could address these layout details while retaining the same basic system architecture. Such a revision is a **potential improvement**, not a completed or scheduled part of this prototype.

The assembled modules' electrical compatibility, display drive margins, and complete system behaviour also require verification during hardware and firmware testing. These notes therefore describe the implemented **design and prototype**, without claiming production readiness or fully verified functionality.
