#!/usr/bin/env python3
"""Draw the wiring diagram, docs/wiring.svg.

    python3 tools/draw_wiring.py docs/wiring.svg

A schematic, not a picture of the breadboard: the chip in the middle with
its 40 pins in their real order (top view, notch up), every part drawn on
the pin it connects to. Plain SVG with no dependencies; edit the layout
below and run it again if the wiring changes.
"""
import sys

W, H = 1200, 960
INK, GREY, PAPER = "#222", "#777", "#fff"
LED_FILL = {"yellow": "#f2c200", "green": "#2a9d3a", "red": "#d43c2c"}

CL, CR = 520, 740          # chip body, left and right edges
TOP, BOT = 140, 750
STUB = 30                  # pin legs

LEFT = ["PB0", "PB1", "PB2", "PB3", "PB4", "PB5 MOSI", "PB6 MISO", "PB7 SCK",
        "RESET", "VCC", "GND", "XTAL2", "XTAL1", "PD0 RXD0", "PD1 TXD0",
        "PD2", "PD3", "PD4", "PD5", "PD6"]
RIGHT = ["PD7", "PC0", "PC1", "PC2", "PC3", "PC4", "PC5", "PC6", "PC7",
         "AVCC", "GND", "AREF", "PA7", "PA6", "PA5", "PA4", "PA3", "PA2",
         "PA1", "PA0"]             # pins 21..40

out = []


def y(pin):
    """The y of a pin: 1-20 down the left side, 21-40 up the right."""
    return 160 + (pin - 1) * 30 if pin <= 20 else 160 + (40 - pin) * 30


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, yy, s, size=12, anchor="start", colour=INK, weight="normal", extra=""):
    out.append(f'<text x="{x}" y="{yy}" font-size="{size}" text-anchor="{anchor}" '
               f'fill="{colour}" font-weight="{weight}"{extra}>{esc(s)}</text>')


def line(x1, y1, x2, y2, colour=INK, width=1.6):
    out.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{colour}" stroke-width="{width}"/>')


def dot(x, yy):
    out.append(f'<circle cx="{x}" cy="{yy}" r="3.2" fill="{INK}"/>')


def gnd(x, yy):
    """Ground symbol hanging below (x, yy)."""
    line(x, yy, x, yy + 10)
    for i, half in enumerate((11, 7, 3)):
        line(x - half, yy + 10 + i * 4, x + half, yy + 10 + i * 4)


def plus5(x, yy, anchor):
    text(x, yy + 4, "+5V", 12, anchor, weight="bold")


def resistor_h(x1, x2, yy, label, below=False):
    """Zigzag between x1 and x2 (leads included), label above (or below)."""
    a, b = x1 + 10, x2 - 10
    pts = [(x1, yy), (a, yy)]
    n = 6
    for i in range(n):
        pts.append((a + (b - a) * (i + 0.5) / n, yy + (-6 if i % 2 == 0 else 6)))
    pts += [(b, yy), (x2, yy)]
    out.append('<polyline fill="none" stroke="%s" stroke-width="1.6" points="%s"/>'
               % (INK, " ".join(f"{px:.1f},{py:.1f}" for px, py in pts)))
    text((x1 + x2) / 2, yy + 20 if below else yy - 10, label, 10, "middle", GREY)


def cap_h(xc, yy, label):
    """Capacitor across a horizontal wire at xc (the wire is drawn by the caller, broken here)."""
    out.append(f'<rect x="{xc - 4}" y="{yy - 3}" width="8" height="6" fill="{PAPER}"/>')
    line(xc - 4, yy - 9, xc - 4, yy + 9, width=2.2)
    line(xc + 4, yy - 9, xc + 4, yy + 9, width=2.2)
    text(xc, yy - 13, label, 10, "middle", GREY)


def cap_v(x, y1, y2, label, label_x, anchor):
    mid = (y1 + y2) / 2
    line(x, y1, x, mid - 4)
    line(x, mid + 4, x, y2)
    line(x - 9, mid - 4, x + 9, mid - 4, width=2.2)
    line(x - 9, mid + 4, x + 9, mid + 4, width=2.2)
    dot(x, y1)
    dot(x, y2)
    text(label_x, mid + 4, label, 10, anchor, GREY)


def led_h(x1, x2, yy, colour, pointing):
    """LED between x1 and x2, current flowing in `pointing` direction (+1 right, -1 left)."""
    a, k = (x1, x2) if pointing > 0 else (x2, x1)     # anode, cathode x
    out.append(f'<polygon points="{a},{yy - 9} {a},{yy + 9} {k},{yy}" fill="{LED_FILL[colour]}" '
               f'stroke="{INK}" stroke-width="1.6"/>')
    line(k, yy - 9, k, yy + 9, width=2.2)
    cx = (a + k) / 2
    for dx in (-3, 5):           # the light: two little arrows
        x0, y0 = cx + dx, yy - 11
        line(x0, y0, x0 + 7, y0 - 8, width=1.2)
        out.append(f'<polygon points="{x0 + 7},{y0 - 8} {x0 + 2},{y0 - 6} {x0 + 6},{y0 - 3}" fill="{INK}"/>')


def button_v(x, y1, y2, label):
    """Push button in a vertical wire from y1 down to y2."""
    t1, t2 = y1 + (y2 - y1) * 0.3, y1 + (y2 - y1) * 0.7
    line(x, y1, x, t1)
    line(x, t2, x, y2)
    for t in (t1, t2):
        out.append(f'<circle cx="{x}" cy="{t}" r="2.6" fill="{PAPER}" stroke="{INK}" stroke-width="1.4"/>')
    bx = x - 9                                    # the plunger, beside the contacts
    line(bx, t1 - 2, bx, t2 + 2, width=2.2)
    line(bx, (t1 + t2) / 2, bx - 9, (t1 + t2) / 2, width=1.6)
    line(bx - 9, (t1 + t2) / 2 - 5, bx - 9, (t1 + t2) / 2 + 5, width=1.6)
    text(bx - 16, (t1 + t2) / 2 + 4, label, 11, "end")


def crystal_v(x, y1, y2, label):
    mid = (y1 + y2) / 2
    line(x, y1, x, mid - 8)
    line(x, mid + 8, x, y2)
    line(x - 8, mid - 8, x + 8, mid - 8, width=2.2)
    line(x - 8, mid + 8, x + 8, mid + 8, width=2.2)
    out.append(f'<rect x="{x - 5}" y="{mid - 5}" width="10" height="10" fill="{PAPER}" stroke="{INK}" stroke-width="1.6"/>')
    dot(x, y1)
    dot(x, y2)
    text(x + 14, mid + 4, label, 10, "start", GREY)


def wire_left(pin, x_end):
    line(CL - STUB, y(pin), x_end, y(pin))


def wire_right(pin, x_end):
    line(CR + STUB, y(pin), x_end, y(pin))


def box(x, yy, w, h, title, rows):
    out.append(f'<rect x="{x}" y="{yy}" width="{w}" height="{h}" rx="6" fill="none" stroke="{INK}" stroke-width="1.4"/>')
    text(x + 12, yy + 22, title, 13, weight="bold")
    for i, r in enumerate(rows):
        text(x + 12, yy + 44 + i * 17, r, 12, colour=GREY if r.startswith("(") else INK)


# ---- the page ----
out.append(f'<rect width="{W}" height="{H}" fill="{PAPER}"/>')
text(40, 50, "megaminer wiring", 22, weight="bold")
text(40, 74, "ATmega1284P-PU, top view (notch up). +5V and GND are the breadboard's power rails.", 13, colour=GREY)

# chip body, notch, legs, names, numbers
out.append(f'<rect x="{CL}" y="{TOP}" width="{CR - CL}" height="{BOT - TOP}" rx="4" fill="#f4f4f4" stroke="{INK}" stroke-width="2"/>')
mx = (CL + CR) / 2
out.append(f'<path d="M {mx - 14} {TOP} A 14 14 0 0 0 {mx + 14} {TOP}" fill="{PAPER}" stroke="{INK}" stroke-width="2"/>')
text(mx, (TOP + BOT) / 2, "ATmega1284P", 16, "middle", GREY, "bold",
     f' transform="rotate(-90 {mx} {(TOP + BOT) / 2})"')
for i, name in enumerate(LEFT):
    p = i + 1
    line(CL - STUB, y(p), CL, y(p), width=2.4)
    text(CL + 8, y(p) + 4, name, 12)
    text(CL - STUB / 2, y(p) - 5, str(p), 9, "middle", GREY)
for i, name in enumerate(RIGHT):
    p = i + 21
    line(CR, y(p), CR + STUB, y(p), width=2.4)
    text(CR - 8, y(p) + 4, name, 12, "end")
    text(CR + STUB / 2, y(p) - 5, str(p), 9, "middle", GREY)

# pin 1: heartbeat LED
wire_left(1, 470)
resistor_h(420, 470, y(1), "220 Ω")
line(370, y(1), 420, y(1))
led_h(340, 370, y(1), "yellow", -1)
line(300, y(1), 340, y(1))
gnd(300, y(1))
text(288, y(1) + 4, "heartbeat (yellow)", 11, "end")

# pins 2-8: the WiFi breakout
bx, bt, bw, bb = 110, 172, 140, 385
out.append(f'<rect x="{bx}" y="{bt}" width="{bw}" height="{bb - bt}" rx="6" fill="#eef4fb" stroke="{INK}" stroke-width="1.6"/>')
text(bx + 8, bt + 20, "ATWINC1500", 12, weight="bold")
text(bx + 8, bt + 36, "WiFi breakout", 11)
text(bx + 8, bt + 51, "(Adafruit 2999)", 10, colour=GREY)
for pin, label in ((2, "EN"), (3, "IRQ"), (4, "RST"), (5, "CS"), (6, "MOSI"), (7, "MISO"), (8, "SCK")):
    text(bx + bw - 6, y(pin) + 4, label, 11, "end")
    if pin in (7, 8):
        wire_left(pin, 400)
        resistor_h(340, 400, y(pin), "220 Ω")
        line(bx + bw, y(pin), 340, y(pin))
    else:
        wire_left(pin, bx + bw)
for pin in (6, 7, 8, 9):                       # where the USBasp joins
    dot(470, y(pin))
    text(470, y(pin) + 15, "ISP", 9, "middle", GREY)
vin_y, gnd_y = 250, 280
text(bx + 6, vin_y + 4, "Vin", 11)
text(bx + 6, gnd_y + 4, "GND", 11)
line(70, vin_y, bx, vin_y)
plus5(66, vin_y, "end")
line(80, gnd_y, bx, gnd_y)
gnd(80, gnd_y)
text(bx + 8, 318, "WAKE, CFG,", 10, colour=GREY)
text(bx + 8, 332, "RXD, TXD:", 10, colour=GREY)
text(bx + 8, 346, "not connected", 10, colour=GREY)

# pin 9: reset pull-up and button
wire_left(9, 280)
resistor_h(220, 280, y(9), "10 kΩ", below=True)
plus5(215, y(9), "end")
dot(300, y(9))
button_v(300, y(9), 460, "reset")
gnd(300, 460)

# pins 10, 11: power, with the 0.1 µF between them
wire_left(10, 450)
plus5(446, y(10), "end")
wire_left(11, 440)
gnd(440, y(11))
cap_v(475, y(10), y(11), "0.1 µF", 462, "end")

# pins 12, 13: crystal and its two 27 pF capacitors
for pin, gx in ((12, 330), (13, 300)):
    wire_left(pin, gx)
    cap_h(370, y(pin), "27 pF")
    gnd(gx, y(pin))
crystal_v(420, y(12), y(13), "20 MHz")

# pins 14, 15: the optional serial adapter
wire_left(14, 440)
text(434, y(14) + 4, "serial adapter TXD", 11, "end", GREY)
wire_left(15, 440)
text(434, y(15) + 4, "serial adapter RXD", 11, "end", GREY)

# pin 16: recovery button
wire_left(16, 420)
button_v(420, y(16), 660, "recovery button")
gnd(420, 660)

# right side: the seven lights on port A
for pin, label, colour in ((40, "telnet (yellow)", "yellow"),
                           (39, "traffic: green", "green"),
                           (38, "traffic: yellow", "yellow"),
                           (37, "traffic: red", "red"),
                           (36, "luck 1 (green)", "green"),
                           (35, "luck 2 (green)", "green"),
                           (34, "luck 3 (green)", "green")):
    wire_right(pin, 810)
    resistor_h(810, 870, y(pin), "220 Ω")
    line(870, y(pin), 905, y(pin))
    led_h(905, 935, y(pin), colour, +1)
    line(935, y(pin), 975, y(pin))
    gnd(975, y(pin))
    text(995, y(pin) + 4, label, 12)

text(CR + STUB + 10, y(33) + 4, "unused (pull-up on)", 11, colour=GREY)
text(CR + STUB + 10, y(32) + 4, "not connected", 11, colour=GREY)

# pins 30, 31: AVCC (also powers port A, so the lights) and GND
wire_right(31, 900)
gnd(900, y(31))
wire_right(30, 820)
plus5(824, y(30), "start")
cap_v(790, y(31), y(30), "0.1 µF", 800, "start")

# pins 24-27: JTAG
line(CR + STUB + 12, y(27) - 6, CR + STUB + 12, y(24) + 6, GREY, 1.2)
text(CR + STUB + 20, (y(27) + y(24)) / 2 - 3, "JTAG (enabled by the fuses):", 11, colour=GREY)
text(CR + STUB + 20, (y(27) + y(24)) / 2 + 13, "not usable as I/O", 11, colour=GREY)

# connectors
box(40, 790, 330, 150, "USBasp (the first flash only)", [
    "MOSI  →  pin 6",
    "MISO  →  pin 7 (chip side of the 220 Ω)",
    "SCK   →  pin 8 (chip side of the 220 Ω)",
    "RST   →  pin 9",
    "GND   →  GND",
    "(VCC not connected: one power source)"])
box(395, 790, 330, 150, "USB-serial adapter, 5 V (recommended)", [
    "TXD  →  pin 14 (PD0, the chip's RXD0)",
    "RXD  →  pin 15 (PD1, the chip's TXD0)",
    "GND  →  GND",
    "(VCC not connected)",
    "250000 baud, 8N1"])
box(750, 790, 410, 150, "Power", [
    "USB-C breadboard power module, both rails 5 V.",
    "Nothing else powers the board: not the USBasp,",
    "not the serial adapter.",
    "Every light: pin → 220 Ω → LED long leg (anode),",
    "short leg (cathode) → GND."])

svg = ('<?xml version="1.0" encoding="UTF-8"?>\n'
       f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
       'font-family="Segoe UI, Helvetica, Arial, sans-serif">\n'
       + "\n".join(out) + "\n</svg>\n")
with open(sys.argv[1] if len(sys.argv) > 1 else "docs/wiring.svg", "w", encoding="utf-8", newline="\n") as f:
    f.write(svg)
