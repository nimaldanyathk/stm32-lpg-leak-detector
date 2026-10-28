#!/usr/bin/env python3
"""
make_schematic.py - draws docs/schematic/lpg_detector_schematic.svg
Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.

Usage:
    python make_schematic.py lpg_detector_schematic.svg

Plain Python, no dependencies. Edit the coordinates below and rerun to
update the drawing. Pin names and values must match firmware/main.c.
"""
import sys

W, H = 1400, 1000
INK, GREY, FILL = "#111111", "#555555", "#ffffff"
FONT = "'Helvetica Neue', Helvetica, Arial, 'Liberation Sans', sans-serif"
out = []
def add(s): out.append(s)
def esc(s): return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
def text(x, y, s, size=11, anchor="start", weight="normal", fill=INK, style="normal", ls=None):
    extra = f" letter-spacing='{ls}'" if ls else ""
    add(f"<text x='{x}' y='{y}' font-size='{size}' text-anchor='{anchor}' font-weight='{weight}' font-style='{style}' fill='{fill}'{extra}>{esc(s)}</text>")
def line(x1, y1, x2, y2, w=1.5, dash=None):
    d = f" stroke-dasharray='{dash}'" if dash else ""
    add(f"<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='{INK}' stroke-width='{w}' stroke-linecap='round'{d}/>")
def wire(pts, w=1.5):
    p = " ".join(f"{x},{y}" for x, y in pts)
    add(f"<polyline points='{p}' fill='none' stroke='{INK}' stroke-width='{w}' stroke-linejoin='round' stroke-linecap='round'/>")
def rect(x, y, w, h, sw=1.5, fill=FILL, dash=None):
    d = f" stroke-dasharray='{dash}'" if dash else ""
    add(f"<rect x='{x}' y='{y}' width='{w}' height='{h}' fill='{fill}' stroke='{INK}' stroke-width='{sw}'{d}/>")
def dot(x, y, r=3.5): add(f"<circle cx='{x}' cy='{y}' r='{r}' fill='{INK}'/>")
def gnd(x, y):
    line(x, y, x, y + 10); line(x - 12, y + 10, x + 12, y + 10, 1.8); line(x - 7, y + 15, x + 7, y + 15, 1.8); line(x - 2, y + 20, x + 2, y + 20, 1.8)
def pflag(x, y, label):
    line(x, y, x, y - 16); line(x - 8, y - 16, x + 8, y - 16, 2.2); text(x, y - 22, label, 10, "middle", "bold")
def res_h(x, y, ref, val, w=60, h=16):
    rect(x, y - h / 2, w, h); text(x + w / 2, y - 13, ref, 10, "middle"); text(x + w / 2, y + h / 2 + 12, val, 10, "middle")
def res_v(x, y, ref, val, h=60, w=16):
    rect(x - w / 2, y, w, h); text(x + 13, y + h / 2 - 2, ref, 10); text(x + 13, y + h / 2 + 11, val, 10)
def nc(x, y, note=None):
    line(x - 4, y - 4, x + 4, y + 4, 1.2); line(x - 4, y + 4, x + 4, y - 4, 1.2)
    text(x, y - 7, note or "NC", 9, "middle", fill=GREY)
def module(x, y, w, h, ref, name, desc, pins, side):
    rect(x, y, w, h)
    if side == "R":
        text(x + 10, y + 17, ref, 13, "start", "bold"); text(x + 10, y + 33, name, 11)
        if desc: text(x + 10, y + 48, desc, 9.5, fill=GREY)
    else:
        text(x + w - 10, y + 17, ref, 13, "end", "bold"); text(x + w - 10, y + 33, name, 11, "end")
        if desc: text(x + w - 10, y + 48, desc, 9.5, "end", fill=GREY)
    for lab, py, kind in pins:
        if side == "R":
            line(x + w, py, x + w + 40, py); text(x + w - 6, py + 4, lab, 10.5, "end")
            if kind == "nc": nc(x + w + 40, py)
        else:
            line(x, py, x - 40, py); text(x + 6, py + 4, lab, 10.5)
            if kind == "nc": nc(x - 40, py, "NC" if kind == "nc" else None)

# ---------------- sheet frame ----------------
add(f"<rect x='0' y='0' width='{W}' height='{H}' fill='{FILL}'/>")
rect(14, 14, W - 28, H - 28, 1); rect(40, 40, W - 80, H - 80, 1.5)
cols, rows = 8, 6
cw, rh = (W - 80) / cols, (H - 80) / rows
for i in range(cols):
    x0 = 40 + i * cw
    if i: line(x0, 14, x0, 40, 0.8); line(x0, H - 40, x0, H - 14, 0.8)
    text(x0 + cw / 2, 31, str(i + 1), 10, "middle"); text(x0 + cw / 2, H - 21, str(i + 1), 10, "middle")
for i in range(rows):
    y0 = 40 + i * rh
    if i: line(14, y0, 40, y0, 0.8); line(W - 40, y0, W - 14, y0, 0.8)
    text(27, y0 + rh / 2 + 4, "ABCDEF"[i], 10, "middle"); text(W - 27, y0 + rh / 2 + 4, "ABCDEF"[i], 10, "middle")
text(52, 62, "SMART LPG LEAK DETECTOR", 13, "start", "bold", ls=1)
text(52, 78, "Wiring schematic, sheet 1 of 1. Team 7, 23CSE304 Embedded Systems. Author: Nimal Danyath K (nimaldanyathk)", 10, fill=GREY)

# ---------------- U1 Black Pill ----------------
MX, MY, MW, MH = 560, 100, 300, 670
rect(MX, MY, MW, MH, 2.2)
text(710, 150, "U1", 14, "middle", "bold"); text(710, 168, "STM32F401CCU6", 12, "middle"); text(710, 184, "Black Pill development board", 10, "middle", fill=GREY)
for x, lab, flag in ((640, "5V", "+5V"), (780, "3V3", "+3V3")):
    line(x, MY, x, MY - 25); pflag(x, MY - 25, flag); text(x, MY + 14, lab, 10.5, "middle", "bold")
line(710, MY + MH, 710, MY + MH + 25); gnd(710, MY + MH + 25); text(710, MY + MH - 6, "GND", 10.5, "middle", "bold")
for y, lab, fn in ((170, "PA1", "ADC1_IN1"), (350, "PB0", "ADC1_IN8"), (530, "PB1", "ADC1_IN9")):
    line(MX, y, MX - 40, y); text(MX + 6, y + 4, lab, 10.5, "start", "bold"); text(MX - 8, y + 15, fn, 9, "end", fill=GREY, style="italic")
for y, lab, fn in ((190, "PB6", "I2C1_SCL"), (220, "PB7", "I2C1_SDA"), (430, "PB12", "BUZZER"), (540, "PA9", "USART1_TX"),
                  (570, "PA10", "USART1_RX"), (710, "PA13", "SWDIO"), (740, "PA14", "SWCLK")):
    line(MX + MW, y, MX + MW + 40, y); text(MX + MW - 6, y + 4, lab, 10.5, "end", "bold"); text(MX + MW + 8, y - 6, fn, 9, fill=GREY, style="italic")
text(710, 586, "HSI 16 MHz internal clock, no PLL", 9.5, "middle", fill=GREY)
text(710, 600, "ADC1 12-bit, 10 Hz sampling", 9.5, "middle", fill=GREY)
# on-board parts
line(MX + 10, 622, MX + MW - 10, 622, 1, dash="4,3"); text(MX + 10, 638, "Fitted on the Black Pill", 9.5, fill=GREY, style="italic")
text(580, 679, "PA0", 10.5, "start", "bold"); line(606, 675, 628, 675)
dot(628, 675, 2.5); dot(672, 675, 2.5); line(630, 674, 668, 661, 1.5); line(672, 675, 700, 675); gnd(700, 675)
text(650, 657, "SW1 KEY (internal pull-up)", 9.5, "middle")
text(580, 726, "3V3", 10.5, "start", "bold"); line(606, 722, 618, 722); rect(618, 716, 36, 12, 1.2); line(654, 722, 670, 722)
add(f"<polygon points='670,714 670,730 692,722' fill='{FILL}' stroke='{INK}' stroke-width='1.5'/>"); line(692, 714, 692, 730)
line(696, 722, 712, 722); text(716, 726, "PC13", 10.5, "start", "bold")
for dx in (0, 7):
    line(676 + dx, 711, 684 + dx, 703, 1); line(680 + dx, 703, 684 + dx, 703, 1); line(684 + dx, 703, 684 + dx, 707, 1)
text(660, 745, "D1 LED, on when PC13 is low", 9.5, "middle")

# ---------------- MQ sensor modules and dividers ----------------
mods = (("U2", "MQ-6 gas sensor module", "LPG, butane, propane", 120),
        ("U3", "MQ-2 gas sensor module", "smoke, combustible gas", 300),
        ("U4", "MQ-135 gas sensor module", "alcohol, VOC, air quality", 480))
for i, (ref, name, desc, top) in enumerate(mods):
    vcc, ao, do, g = top + 20, top + 50, top + 80, top + 110
    module(70, top, 190, 130, ref, name, desc, [("VCC", vcc, "sig"), ("AO", ao, "sig"), ("DO", do, "nc"), ("GND", g, "sig")], "R")
    wire([(300, vcc), (320, vcc), (320, vcc - 20)]); pflag(320, vcc - 20, "+5V")
    gnd(300, g)
    line(300, ao, 320, ao); res_h(320, ao, f"R{2*i+1}", "10k"); line(380, ao, 440, ao); dot(440, ao); line(440, ao, 520, ao)
    line(440, ao, 440, ao + 10); res_v(440, ao + 10, f"R{2*i+2}", "10k"); line(440, ao + 70, 440, ao + 85); gnd(440, ao + 85)
    text(290, ao - 6, "0 to 5 V", 9, "middle", fill=GREY, style="italic")
    text(480, ao - 6, "0 to 2.5 V", 9, "middle", fill=GREY, style="italic")

# ---------------- PS1 power ----------------
module(70, 660, 190, 80, "PS1", "5 V USB wall adapter", "1 A or more", [("+5V", 690, "sig"), ("GND", 720, "sig")], "R")
wire([(300, 690), (320, 690), (320, 670)]); pflag(320, 670, "+5V"); gnd(300, 720)
text(70, 760, "Plugs into the Black Pill USB-C port. The board's 5V header pin", 9, fill=GREY, style="italic")
text(70, 772, "feeds the MQ heaters and the HC-05; 3V3 comes from the on-board regulator.", 9, fill=GREY, style="italic")

# ---------------- right side modules ----------------
RX, RW = 1110, 230
module(RX, 110, RW, 130, "U5", "SSD1306 OLED 0.96 in", "I2C address 0x3C, 3.3 V",
       [("VCC", 130, "sig"), ("GND", 160, "sig"), ("SCL", 190, "sig"), ("SDA", 220, "sig")], "L")
wire([(1070, 130), (1040, 130), (1040, 110)]); pflag(1040, 110, "+3V3"); line(1070, 160, 1040, 160); gnd(1040, 160)
module(RX, 280, RW, 130, "U6", "AHT20 temperature / humidity", "I2C address 0x38, 3.3 V",
       [("VCC", 300, "sig"), ("GND", 330, "sig"), ("SDA", 360, "sig"), ("SCL", 390, "sig")], "L")
wire([(1070, 300), (1040, 300), (1040, 280)]); pflag(1040, 280, "+3V3"); line(1070, 330, 1040, 330); gnd(1040, 330)
# I2C bus
line(900, 190, 1070, 190); line(900, 220, 1070, 220)
dot(960, 190); line(960, 190, 960, 390); line(960, 390, 1070, 390)
dot(1000, 220); line(1000, 220, 1000, 360); line(1000, 360, 1070, 360)
module(RX, 460, RW, 190, "U7", "HC-05 Bluetooth module", "UART 9600 8N1, 5 V",
       [("EN", 480, "nc"), ("VCC", 510, "sig"), ("RXD", 540, "sig"), ("TXD", 570, "sig"), ("GND", 600, "sig"), ("STATE", 630, "nc")], "L")
wire([(1070, 510), (1040, 510), (1040, 490)]); pflag(1040, 490, "+5V"); line(1070, 600, 1040, 600); gnd(1040, 600)
line(900, 540, 1070, 540); line(900, 570, 1070, 570)
module(RX, 690, RW, 130, "J2", "SWD to ST-Link V2", "SWDIO, SWCLK, GND only",
       [("SWDIO", 710, "sig"), ("SWCLK", 740, "sig"), ("GND", 770, "sig"), ("3V3", 800, "nc")], "L")
line(900, 710, 1070, 710); line(900, 740, 1070, 740); line(1070, 770, 1040, 770); gnd(1040, 770)
text(1030, 804, "do not connect", 8.5, "end", fill=GREY, style="italic")
# buzzer
wire([(900, 430), (980, 430), (980, 450)])
add(f"<circle cx='980' cy='470' r='20' fill='{FILL}' stroke='{INK}' stroke-width='1.5'/>")
add(f"<path d='M 968 470 A 12 12 0 0 1 992 470' fill='none' stroke='{INK}' stroke-width='1.5'/>"); line(968, 470, 992, 470, 1.5)
text(996, 452, "+", 11, "middle", "bold")
line(980, 490, 980, 510); gnd(980, 510)
text(952, 466, "BZ1", 10, "end", "bold"); text(952, 480, "active buzzer", 9.5, "end")

# ---------------- notes ----------------
line(40, 840, 1360, 840, 1.5); line(940, 840, 940, 960, 1.5)
text(50, 856, "NOTES", 11, "start", "bold", ls=1)
notes = [
 "1. R1 to R6: 10 kOhm. Each pair halves the 0 to 5 V sensor output, so the ADC pins see at most 2.5 V (3.3 V limit).",
 "2. MQ-6, MQ-2, MQ-135 and HC-05 run from the 5 V rail. Each MQ heater draws about 150 mA, so use a 5 V adapter rated 1 A or more.",
 "3. SSD1306 and AHT20 run from the Black Pill 3.3 V regulator and share I2C1. Internal pull-ups on PB6/PB7 are enabled in firmware; none are fitted externally.",
 "4. SWD: connect SWDIO, SWCLK and GND only. Leave the ST-Link 3.3 V pin open while the board is powered over USB.",
 "5. SW1 (KEY) and D1 (LED) are fitted on the Black Pill. D1 is active low. DO pins of the MQ modules are not used.",
 "6. Firmware: HSI 16 MHz, sensors sampled at 10 Hz, USART1 9600 8N1. Alarm logic is described in the README.",
]
for i, n in enumerate(notes): text(50, 871 + i * 13.4, n, 9.5)
dot(56, 952, 3); text(64, 955, "wire junction.   Crossing wires without a dot are not connected.   NC = not connected.", 9, fill=GREY)
text(930, 955, "Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.", 8.5, "end", fill=GREY)

# ---------------- title block ----------------
TX = 940
line(TX, 876, 1360, 876, 1); line(TX, 904, 1360, 904, 1); line(TX, 932, 1360, 932, 1)
line(1240, 840, 1240, 960, 1); line(1040, 904, 1040, 932, 1)
text(TX + 12, 865, "SMART LPG LEAK DETECTOR", 17, "start", "bold", ls=1)
text(1248, 852, "SHEET", 7.5, fill=GREY, ls=1); text(1248, 868, "1 / 1", 12, "start", "bold")
text(1310, 852, "SIZE", 7.5, fill=GREY, ls=1); text(1310, 868, "A3", 12, "start", "bold")
text(TX + 12, 886, "TITLE", 7.5, fill=GREY, ls=1); text(TX + 12, 898, "Wiring schematic, STM32F401CCU6 Black Pill", 10.5)
text(1248, 886, "REV", 7.5, fill=GREY, ls=1); text(1248, 898, "A", 11, "start", "bold")
text(TX + 12, 914, "DRAWN BY", 7.5, fill=GREY, ls=1); text(TX + 12, 926, "Team 7", 10.5, "start", "bold")
text(1048, 914, "AUTHOR", 7.5, fill=GREY, ls=1); text(1048, 926, "Nimal Danyath K", 10.5)
text(1248, 914, "DATE", 7.5, fill=GREY, ls=1); text(1248, 926, "2026-10-15", 10.5)
text(TX + 12, 942, "PROJECT", 7.5, fill=GREY, ls=1); text(TX + 12, 954, "23CSE304 Embedded Systems, Amrita Vishwa Vidyapeetham", 9.5)
text(1248, 942, "DOCUMENT NO", 7.5, fill=GREY, ls=1); text(1248, 954, "LPG-SCH-001", 10.5)

svg = (f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' width='{W}' height='{H}' font-family=\"{FONT}\" role='img' aria-labelledby='t'>\n"
       "<title id='t'>Smart LPG leak detector, wiring schematic, STM32F401CCU6 Black Pill</title>\n"
       "<!-- Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved. -->\n" + "\n".join(out) + "\n</svg>\n")
open(sys.argv[1], "w").write(svg)
print("wrote", sys.argv[1], len(svg), "bytes")
