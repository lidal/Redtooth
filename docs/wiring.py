"""Draws wiring.svg: python wiring.py

The Feather is the breadboard drawing from Adafruit's Fritzing part
(github.com/adafruit/Fritzing-Library, CC BY-SA 3.0), downloaded on each run.
"""
import io
import re
import urllib.request
import zipfile
from pathlib import Path

PART = ("https://raw.githubusercontent.com/adafruit/Fritzing-Library/1899fe023baa0bafbf9cb02e74efcfaa662d3f9a/"
        "parts/Adafruit%20Feather%20ESP32-C6.fzpz")
# Fritzing connector ids of the pins used
PINS = {"IO5": 62, "IO6": 61, "IO7": 60, "IO8": 59, "3V": 51, "GND": 49, "BAT+": 193, "BAT-": 192}

SCALE = 4
BX, BY = 40, 150  # top left corner of the Feather
CABLE_X, CABLE_Y = 760, 288
FONT = 'font-family="Helvetica, Arial, sans-serif"'

# Wires of the original controller cable, outermost first. The top ones go to the
# header along the top of the Feather, the bottom ones to the header along the bottom.
TOP = [
    ("yellow", "#e6b800", "Data", "IO8"),
    ("red", "#d42020", "power switch", "IO7"),
    ("orange", "#f08c00", "Clock", "IO6"),
    ("brown", "#8b4513", "Latch", "IO5"),
]
BOTTOM = [
    ("blue", "#1f5fd6", "+5V", "3V"),
    ("black", "#111111", "GND", "GND"),
]


def load_board():
    data = urllib.request.urlopen(PART).read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        name = next(n for n in z.namelist() if n.endswith("breadboard.svg"))
        svg = z.read(name).decode("utf-8")
    view_w, view_h = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', svg).groups())
    pins = {}
    for pin, n in PINS.items():
        tag = re.search(r'<circle[^>]*id="connector%dpin"[^>]*>' % n, svg).group(0)
        cx, cy = (float(re.search(r'\b%s="([-\d.]+)"' % k, tag).group(1)) for k in ("cx", "cy"))
        pins[pin] = (BX + cx * SCALE, BY + cy * SCALE)

    body = svg[svg.index(">", svg.index("<svg")) + 1:svg.rindex("</svg>")]
    body = re.sub(r"<desc>.*?</desc>|<metadata.*?</metadata>|<!--.*?-->", "", body, flags=re.S)
    body = re.sub(r'\s(gorn|id)="[^"]*"', "", body)
    # 0.01 drawing units is 0.04 px here, so two decimals are plenty
    body = re.sub(r'\b(d|points|x|y|x1|y1|x2|y2|cx|cy|r|rx|ry|width|height)="([^"]*)"',
                  lambda m: '%s="%s"' % (m.group(1), round_numbers(m.group(2))), body)
    body = merge_rects(re.sub(r">\s+<", "><", body).strip())
    drawing = ('<svg x="%g" y="%g" width="%g" height="%g" viewBox="0 0 %g %g">%s</svg>'
               % (BX, BY, view_w * SCALE, view_h * SCALE, view_w, view_h, body))
    return drawing, pins, BY + view_h * SCALE


def round_numbers(path):
    number = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
    return re.sub(number, lambda m: " %s" % ("%.2f" % float(m.group(0))).rstrip("0").rstrip("."), path).strip()


def merge_rects(body):
    """Joins runs of plain rects with one fill into a path: the silkscreen text is ~7500 thin rects."""
    rect = r'<rect width="([-\d.]+)" height="([-\d.]+)" y="([-\d.]+)" x="([-\d.]+)" fill="(#\w+)" />'

    def join(run):
        paths, fill, d = [], None, []
        for w, h, y, x, f in re.findall(rect, run.group(0)):
            if f != fill and d:
                paths.append('<path fill="%s" d="%s"/>' % (fill, "".join(d)))
                d = []
            fill = f
            d.append("M%s %sh%sv%sh-%sz" % (x, y, w, h, w))
        paths.append('<path fill="%s" d="%s"/>' % (fill, "".join(d)))
        return "".join(paths)

    return re.sub("(?:%s){2,}" % rect, join, body)


def wire(points, colour):
    pts = " ".join("%g,%g" % p for p in points)
    return ('<polyline points="%s" fill="none" stroke="#000" stroke-opacity=".35" stroke-width="5.5" '
            'stroke-linejoin="round" stroke-linecap="round"/>'
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="3.5" stroke-linejoin="round" '
            'stroke-linecap="round"/><circle cx="%g" cy="%g" r="3.5" fill="%s"/>'
            % (pts, pts, colour, *points[-1], colour))


def text(x, y, s, size=12.5, anchor="start", colour="#222"):
    return '<text x="%g" y="%g" font-size="%g" text-anchor="%s" fill="%s" %s>%s</text>' % (
        x, y, size, anchor, colour, FONT, s)


def main():
    board, pins, board_bottom = load_board()
    out = []
    wires = len(TOP) + len(BOTTOM)
    exits = [CABLE_Y - 4 * (wires - 1) + 8 * i for i in range(wires)]

    for i, (name, colour, signal, pin) in enumerate(TOP):
        lane, turn = 76 + 18 * i, CABLE_X - 34 - 12 * i
        px, py = pins[pin]
        out.append(wire([(CABLE_X + 10, exits[i]), (turn, exits[i]), (turn, lane), (px, lane), (px, py)], colour))
        out.append(text(turn - 8, lane - 4, "%s: %s → %s" % (name, signal, pin), anchor="end"))
    for j, (name, colour, signal, pin) in enumerate(BOTTOM):
        y = exits[len(TOP) + j]
        lane, turn = board_bottom + 50 - 18 * j, CABLE_X - 34 - 12 * (len(BOTTOM) - 1 - j)
        px, py = pins[pin]
        out.append(wire([(CABLE_X + 10, y), (turn, y), (turn, lane), (px, lane), (px, py)], colour))
        out.append(text(turn - 8, lane + (16 if j == 0 else -5), "%s: %s → %s" % (name, signal, pin), anchor="end"))

    # The LiPo plugs into the Feather's JST socket
    (plus_x, plus_y), (minus_x, minus_y) = pins["BAT+"], pins["BAT-"]
    battery = ['<rect x="%g" y="40" width="150" height="56" rx="7" fill="#c9cdd3" stroke="#8a8f96" stroke-width="1.5"/>'
               % (minus_x - 60), text(minus_x + 15, 74, "LiPo", 15, "middle")]
    battery += [wire([(x, 96), (x, y)], c) for x, y, c in ((plus_x, plus_y, "#d42020"), (minus_x, minus_y, "#111111"))]

    cable = ['<rect x="%g" y="%g" width="150" height="%g" rx="9" fill="#2b2b2b"/>'
             % (CABLE_X, exits[0] - 12, exits[-1] - exits[0] + 24),
             text(CABLE_X + 75, exits[-1] + 34, "Virtual Boy", 14, "middle"),
             text(CABLE_X + 75, exits[-1] + 51, "controller cable", 14, "middle")]

    width, height = CABLE_X + 170, board_bottom + 80
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" viewBox="0 0 %g %g">'
           '<rect width="100%%" height="100%%" fill="#fff"/>%s%s%s%s%s</svg>\n'
           % (width, height, width, height, board, "".join(out), "".join(battery), "".join(cable),
              text(10, height - 10, "Feather drawing: Adafruit Fritzing library, CC BY-SA 3.0", 10, colour="#888")))
    Path(__file__).with_name("wiring.svg").write_text(svg, encoding="utf-8")


main()
