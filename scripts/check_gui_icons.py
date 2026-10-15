#!/usr/bin/env python3
"""Check that the GUI's icons draw their straight lines on whole pixels.

A stroke centered on a pixel boundary renders as two pixels at partial
strength, so it looks blurred, and darker and duller than its color.  At the
size an icon is drawn on, its viewBox, each horizontal or vertical stroke must
cover whole pixels:

- a stroke at least 1 and under 2 px wide centers on a pixel center (k + 0.5),
  a 2 px stroke on a pixel boundary, and so on by the stroke's whole pixels;
- the edge of a fill without a stroke lies on a pixel boundary, unless a
  stroke along the same line covers it.

Curves and diagonals antialias anyway and are not checked, nor are straight
runs shorter than a corner.  IconSet.def lists the icons allowed a line
between two pixels with NEVERD_ICON_SOFT_LINE, each with its reason.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

REPOSITORY = Path(__file__).resolve().parents[1]
GUI = REPOSITORY / "tools" / "neverd-gui"
ICONS = GUI / "resources" / "icons"
RECORD = GUI / "app" / "IconSet.def"
SOFT_LINE = re.compile(r'NEVERD_ICON_SOFT_LINE\(\s*"([^"]+)"')
NUMBER = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
TOKEN = re.compile(rf"([MmLlHhVvZzCcSsQqTtAa])|({NUMBER})")
SVG_NAMESPACE = "{http://www.w3.org/2000/svg}"
# The arguments each path command takes per repetition.
ARGUMENTS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2,
             "A": 7, "Z": 0}
# Straight runs shorter than this, in pixels, are corners and caps.
SHORTEST_LINE = 1.5
# How far a line may sit from where it should, in pixels.
TOLERANCE = 0.13


@dataclass(frozen=True)
class Line:
    """A horizontal or vertical segment: the coordinate it runs along (x for
    a vertical line) and the span it covers, in drawn pixels."""
    axis: str
    at: float
    start: float
    end: float

    @property
    def length(self) -> float:
        return self.end - self.start


def path_segments(data: str):
    """The straight segments (x1, y1, x2, y2) of SVG path data, following the
    current point through curves and arcs, which are not segments."""
    tokens = [command or float(number) for command, number in TOKEN.findall(data)]
    x = y = start_x = start_y = 0.0
    command = None
    index = 0
    while index < len(tokens):
        if isinstance(tokens[index], str):
            command = tokens[index]
            index += 1
            if command in "Zz":
                yield (x, y, start_x, start_y)
                x, y = start_x, start_y
                # Only a command may follow a close.
                command = None
                continue
        if command is None:
            raise ValueError("numbers without a command")
        count = ARGUMENTS[command.upper()]
        values = tokens[index:index + count]
        if len(values) < count or any(isinstance(value, str) for value in values):
            raise ValueError(f"command {command} is missing arguments")
        index += count
        relative = command.islower()
        kind = command.upper()
        if kind == "H":
            end_x, end_y = values[0] + (x if relative else 0), y
        elif kind == "V":
            end_x, end_y = x, values[0] + (y if relative else 0)
        else:
            end_x, end_y = values[-2], values[-1]
            if relative:
                end_x, end_y = end_x + x, end_y + y
        if kind in "LHV":
            yield (x, y, end_x, end_y)
        x, y = end_x, end_y
        if kind == "M":
            start_x, start_y = x, y
            # Further pairs after a move are lines.
            command = "l" if relative else "L"


def element_segments(element, tag: str):
    """The straight segments an element draws, in viewBox units."""
    def number(name, default=0.0):
        return float(element.get(name, default))

    if tag == "path":
        return list(path_segments(element.get("d", "")))
    if tag == "rect":
        x, y = number("x"), number("y")
        width, height = number("width"), number("height")
        radius = number("rx", element.get("ry", 0))
        return [(x + radius, y, x + width - radius, y),
                (x + radius, y + height, x + width - radius, y + height),
                (x, y + radius, x, y + height - radius),
                (x + width, y + radius, x + width, y + height - radius)]
    if tag == "line":
        return [(number("x1"), number("y1"), number("x2"), number("y2"))]
    if tag in ("polyline", "polygon"):
        values = [float(value) for value in re.findall(NUMBER, element.get("points", ""))]
        points = list(zip(values[::2], values[1::2]))
        if tag == "polygon" and points:
            points.append(points[0])
        return [(*a, *b) for a, b in zip(points, points[1:])]
    return []


def straight(segment, scale_x: float, scale_y: float):
    """The segment as a Line in drawn pixels, or None when it is not
    horizontal or vertical or is too short to show as a line."""
    x1, y1, x2, y2 = segment
    if x1 == x2 and y1 != y2:
        line = Line("x", x1 * scale_x, min(y1, y2) * scale_y, max(y1, y2) * scale_y)
    elif y1 == y2 and x1 != x2:
        line = Line("y", y1 * scale_y, min(x1, x2) * scale_x, max(x1, x2) * scale_x)
    else:
        return None
    return line if line.length >= SHORTEST_LINE else None


def off_by(value: float, target: float) -> float:
    """How far value is from the nearest k + target."""
    distance = (value - target) % 1.0
    return min(distance, 1.0 - distance)


def stroke_center(width: float) -> float:
    """Where a stroke of width pixels centers to cover whole pixels: on a
    pixel center for an odd count of whole pixels, else on a boundary."""
    return 0.5 if math.floor(width) % 2 == 1 or width < 1 else 0.0


def check_icon(path: Path, allowed: set[str]) -> list[str]:
    """The lines of one icon that fall between two pixels."""
    if path.stem in allowed:
        return []
    root = ET.parse(path).getroot()
    view_box = [float(value) for value in root.get("viewBox", "").split()]
    if len(view_box) != 4:
        return [f"{path.name}: has no viewBox to tell the size it is drawn on"]
    width = float(root.get("width", view_box[2]))
    height = float(root.get("height", view_box[3]))
    scale_x, scale_y = width / view_box[2], height / view_box[3]
    strokes: list[tuple[Line, float, str]] = []
    fills: list[tuple[Line, str]] = []
    problems: list[str] = []

    def walk(element, inherited):
        paint = dict(inherited)
        for name in ("fill", "stroke", "stroke-width"):
            if element.get(name) is not None:
                paint[name] = element.get(name)
        tag = element.tag.replace(SVG_NAMESPACE, "")
        if element.get("transform") is not None:
            problems.append(f"{path.name}: <{tag}> has a transform, which this "
                            "check cannot follow; draw the shape in place")
            return
        stroked = paint.get("stroke", "none") != "none"
        filled = paint.get("fill", "black") != "none"
        stroke_width = float(paint.get("stroke-width", 1))
        try:
            segments = element_segments(element, tag)
        except ValueError as error:
            problems.append(f"{path.name}: <{tag}> path data: {error}")
            segments = []
        for segment in segments:
            line = straight(segment, scale_x, scale_y)
            if line is None:
                continue
            if stroked:
                scale = scale_x if line.axis == "x" else scale_y
                strokes.append((line, stroke_width * scale, tag))
            elif filled:
                fills.append((line, tag))
        for child in element:
            walk(child, paint)

    walk(root, {})
    for line, drawn_width, tag in strokes:
        center = stroke_center(drawn_width)
        if off_by(line.at, center) > TOLERANCE:
            problems.append(
                f"{path.name}: <{tag}> {drawn_width:g} px stroke along "
                f"{line.axis}={line.at:g} ({line.length:g} px long) spreads over "
                f"two pixels; center it on {line.axis}=k+{center:g}")
    for line, tag in fills:
        if off_by(line.at, 0.0) <= TOLERANCE:
            continue
        covered = any(
            stroke.axis == line.axis and abs(stroke.at - line.at) <= TOLERANCE
            and stroke.start <= line.start + 0.5 and stroke.end >= line.end - 0.5
            for stroke, _, _ in strokes)
        if not covered:
            problems.append(
                f"{path.name}: <{tag}> fill edge at {line.axis}={line.at:g} "
                f"({line.length:g} px long) ends inside a pixel; move it to a "
                "whole pixel")
    return problems


def allowed_icons(record: Path) -> set[str]:
    """The icons IconSet.def allows a line between two pixels."""
    return set(SOFT_LINE.findall(record.read_text(encoding="utf-8")))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("icons", nargs="*", type=Path,
                        help="SVG files to check (default: every GUI icon)")
    parser.add_argument("--record", type=Path, default=RECORD,
                        help="the icon record that lists allowed soft lines")
    arguments = parser.parse_args(argv)
    icons = arguments.icons or sorted(ICONS.glob("*.svg"))
    allowed = allowed_icons(arguments.record)
    problems = [problem for icon in icons for problem in check_icon(icon, allowed)]
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems:
        print(f"{len(problems)} line(s) fall between pixels in "
              f"{len({problem.split(':')[0] for problem in problems})} icon(s)",
              file=sys.stderr)
        return 1
    print(f"{len(icons)} icons draw their straight lines on whole pixels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
