# Custom ATmega328P Optical Sensing Board

## Demonstration Video

*A video demonstration will be added here.*

<!-- Replace the line above with a GitHub-hosted user-attachments video URL on its own line. -->

## 1. Hardware Design

### Schematic Design

<p align="center">
  <a href="hardware/schematic/schematic.pdf">
    <img src="images/schematic-preview.png" alt="Circuit schematic" width="400">
  </a>
</p>

I designed the circuit schematic in Altium Designer around the ATmega328P-AU, including its clock and reset circuitry, USB-to-UART interface, sensor and display connections, pushbuttons, and ISP programming interface. The circuit choices are explained in the [Hardware Design Notes](docs/design-notes.md).

### PCB Layout and Routing

<a href="hardware/pcb/pcb-layout.pdf">
  <img src="hardware/pcb/pcb-layout-black.svg" alt="PCB layout and routing" align="right" width="350">
</a>

I translated the schematic into a two-layer PCB in Altium Designer, completing the component placement and trace routing myself. The layout brings the MCU, optical sensor module, displays, buttons, USB-to-UART interface, and ISP connections together on one custom board.

The layout shown here is the one used for the fabricated prototype. It is a practical first implementation rather than a fully optimised PCB; the design decisions and known areas for improvement are discussed in the [Hardware Design Notes](docs/design-notes.md).

<br clear="all">

## 2. PCB Fabrication, Assembly and Bootloader Programming

### Fabrication and Assembly

<p align="center">
  <img src="images/pcb-bare.jpg" alt="Fabricated bare PCB" width="280">
  <img src="images/pcb-assembled.jpg" alt="Assembled PCB before power-on" width="280">
</p>

I had the completed PCB layout fabricated, then populated and soldered the board to assemble the physical prototype. The photographs show the bare PCB and the assembled board before power-on.

### Bootloader Programming

<img src="images/bootloader-flashing-success.jpg" alt="Bootloader programming with a six-pin ISP programmer" align="left" width="270">

I initialised the ATmega328P-AU with a commercial UNO R3 six-pin ISP programmer through the board's temporary ISP contacts. The target was powered via Mini USB, and the programmer's steady green LED indicated a successful Bootloader operation.

After this initial step, the on-board CH340C USB-to-UART interface provides the normal route for uploading application firmware. The connection sequence, orientation, and LED indications are documented in the [Bootloader Programming Setup](docs/programming-setup.md).

<br clear="all">

## 3. Firmware Demonstration

### Application Modes

| RAW Mode | STATS — Measuring | STATS — Idle |
|:---:|:---:|:---:|
| ![RAW mode displaying sensor readings](images/raw-mode.jpg) | ![STATS mode displaying measurements and signal quality](images/stats-measuring.jpg) | ![STATS mode waiting for a finger](images/stats-idle.jpg) |

## 4. Firmware Architecture

## 5. Repository Structure

## 6. Notes and Limitations
