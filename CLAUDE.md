# Project notes for Claude Code

Owner: Nimal Danyath K (GitHub: nimaldanyathk). Course project for 23CSE304 Embedded Systems, Amrita Vishwa Vidyapeetham.

## What this is

LPG leak detector on an STM32F401CCU6 Black Pill. Three MQ gas sensors and an AHT20 feed a small decision tree on the chip, so cooking smoke, steam and alcohol do not set off the alarm but a real LPG leak does. A fail-safe rule (MQ-6 Rs/R0 below 0.40 for 1 s) always alarms regardless of the model.

## Hardware and pins (do not change)

| Device | Pin |
|---|---|
| MQ-6 AO (via 10k/10k divider) | PA1 / ADC1_IN1 |
| MQ-2 AO (via divider) | PB0 / ADC1_IN8 |
| MQ-135 AO (via divider) | PB1 / ADC1_IN9 |
| OLED SSD1306 0x3C + AHT20 0x38 | PB6 SCL, PB7 SDA (I2C1, shared) |
| HC-05 | PA9 TX -> RXD, PA10 RX <- TXD, 9600 baud |
| Buzzer | PB12 |
| LED | PC13 (active low) |
| KEY button | PA0 |
| SWD | PA13/PA14 (ST-Link: SWDIO, SWCLK, GND only) |

MQ sensors and HC-05 run from 5 V. OLED and AHT20 from 3V3.

## Code layout

- `firmware/main.c` + `features.h` + `model.h`: main firmware, Keil MDK + CMSIS, register level, no HAL. Clock forced to HSI 16 MHz.
- `firmware/single_file/main.c`: same firmware merged into one file. Regenerate it if the three files change.
- `ml/train.py`: reads Bluetooth logs (`B,` and `D,` lines), trains a depth-5 decision tree, writes `firmware/model.h`. Its feature maths must match `features.h` exactly.
- `phone_app/gas_monitor.html`: Web Serial dashboard for the HC-05.
- `bringup_tests/`: small step-by-step hardware tests.

## Rules for changes

- Do not change pin assignments, thresholds or alarm logic unless the owner asks.
- Keep comments short and plain. Keep the copyright line at the top of each source file:
  `Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.`
- Do not use em dashes in code comments or docs.
- Keep C code C90-friendly (declarations at the top of blocks) so Keil ARM Compiler 5 and 6 both build it.
- Flash budget: Keil free licence is 32 KB. Current build is about 25 KB.
- Never commit recorded logs from `ml/logs/`.

## Checking a change

If `arm-none-eabi-gcc` is available, a syntax/size check needs the CMSIS headers (STMicroelectronics/cmsis_device_f4 and ARM CMSIS_5 Core). Real testing is on the board in Keil.
