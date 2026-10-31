"""Draws wiring.svg. Needs schemdraw: pip install schemdraw, then python wiring.py"""
import schemdraw
from schemdraw import elements as elm

schemdraw.use("svg")
schemdraw.config(fontsize=13, bgcolor="white")

# Original controller cable: colour, controller signal, Feather pin
WIRES = [
    ("blue", "#1f5fd6", "+5V", "3V"),
    ("brown", "#8b4513", "Latch", "IO5 / A3"),
    ("orange", "#f08c00", "Clock", "IO6 / A2"),
    ("yellow", "#d9a800", "Data", "IO8"),
    ("black", "black", "GND", "GND"),
    ("red: power switch, LOW = off", "#d42020", "Vbat", "IO7"),
]
N = len(WIRES)
GAP = 10

with schemdraw.Drawing(file="wiring.svg", show=False) as d:
    ctl = elm.Ic(
        pins=[elm.IcPin(name=c, side="R", slot=f"{N - i}/{N}", anchorname=f"c{i}")
              for i, (_, _, c, _) in enumerate(WIRES)],
        size=(3, N + 1), leadlen=0.6,
    ).label("Virtual Boy controller", "top")
    fth = elm.Ic(
        pins=[elm.IcPin(name=f, side="L", slot=f"{N - i}/{N}", anchorname=f"f{i}")
              for i, (_, _, _, f) in enumerate(WIRES)]
        + [elm.IcPin(name="BAT", side="R", slot=f"1/{N}", anchorname="bat")],
        size=(3.4, N + 1), leadlen=0.6,
    ).anchor("f0").at((ctl.c0[0] + GAP, ctl.c0[1])).label("Adafruit ESP32-C6 Feather", "top")

    for i, (name, colour, _, _) in enumerate(WIRES):
        elm.Line(lw=3).at(getattr(ctl, f"c{i}")).to(getattr(fth, f"f{i}")).color(colour).label(name, "top", fontsize=11)

    elm.Line().at(fth.bat).right(1.5)
    elm.Battery().down(2).label("LiPo", "bottom", fontsize=11)
    elm.Ground()
