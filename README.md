# Custom ATmega328P Optical Sensing Board

## Demonstration Video

*A video demonstration will be added here.*

<!-- Replace the line above with a GitHub-hosted user-attachments video URL on its own line. -->

## 1. Hardware Design

### Schematic Design

<a href="hardware/schematic/schematic.pdf">
  <img src="images/schematic-preview.png" alt="Circuit schematic" align="right" width="400">
</a>

The project began with a schematic drawn in Altium Designer around the ATmega328P-AU. I brought together the MCU's power, clock and reset circuits, the USB-to-UART and ISP interfaces, and connections for the MAX30102 sensor, OLED, seven-segment display and buttons. This established the electrical foundation for the custom board, with the circuit details covered in the [Hardware Design Notes](docs/design-notes.md).

<br clear="all">

<p align="right"><sub>Click the image to view the complete schematic (PDF).</sub></p>

### PCB Layout and Routing

<a href="hardware/pcb/pcb-layout.pdf">
  <img src="hardware/pcb/pcb-layout-black.svg" alt="PCB layout and routing" align="right" width="350">
</a>

With the circuit connections defined, the next step was turning them into a physical layout. In Altium Designer, I placed the components and routed both PCB layers, bringing the sensing, display, control and programming interfaces together on one board.

This is the actual layout used for fabrication, not a conceptual illustration. It also reflects the practical compromises of an initial prototype; the placement and routing choices, along with possible improvements, are discussed in the [Hardware Design Notes](docs/design-notes.md).

<br clear="all">

<p align="right"><sub>Click the image to view the full PCB layout (PDF).</sub></p>

## 2. PCB Fabrication, Assembly and Bootloader Programming

### Fabrication and Assembly

<p align="center">
  <img src="images/pcb-bare.jpg" alt="Fabricated bare PCB" width="280">
  <img src="images/pcb-assembled.jpg" alt="Assembled PCB before power-on" width="280">
</p>

Once the layout was complete, it was sent for PCB fabrication. The bare board provided the physical starting point; I then populated and soldered the components to assemble the complete prototype. The two photographs capture that progression from manufactured PCB to assembled hardware, before power-on.

### Bootloader Programming

<img src="images/bootloader-flashing-success.jpg" alt="Bootloader programming with a six-pin ISP programmer" align="left" width="270">

An assembled board still needed a way to receive firmware. Before normal serial uploads through Mini USB and the CH340C could work, I used a commercial UNO R3 six-pin ISP programmer to initialise the ATmega328P-AU through the temporary ISP contacts. The target was powered via Mini USB, and the programmer's steady green LED indicated a successful Bootloader operation.

With that initial step complete, the USB-to-UART interface could be used for application uploads. The programming sequence, contact orientation and LED indications are documented in the [Bootloader Programming Setup](docs/programming-setup.md).

<br clear="all">

## 3. Firmware Demonstration

The running application brings sensor readings and user interaction together in two display modes. RAW exposes the red and infrared readings, while STATS presents estimated measurements and signal-quality feedback. The firmware structure and processing methods are described in the [Firmware Overview](docs/firmware-overview.md).

### Application Modes

| RAW Mode | STATS — Measuring | STATS — Idle |
|:---:|:---:|:---:|
| ![RAW mode displaying sensor readings](images/raw-mode.jpg) | ![STATS mode displaying measurements and signal quality](images/stats-measuring.jpg) | ![STATS mode waiting for a finger](images/stats-idle.jpg) |

## 4. Firmware Architecture

## 5. Repository Structure

## 6. Notes and Limitations
