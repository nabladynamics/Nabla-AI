"""Minimal, dependency-free SVG plotting for the validation report.

Three figure kinds cover the whole report: line charts (histories, profiles,
spectra), bar charts (mesh activity, level histograms, cell counts), and rect
maps (field heatmaps and physics-label / mesh slices on irregular cells).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from html import escape

FONT = "font-family='Helvetica,Arial,sans-serif'"


def nice_ticks(lo: float, hi: float, target: int = 5) -> list[float]:
    if hi <= lo:
        return [lo]
    span = hi - lo
    raw = span / max(target, 2)
    mag = 10 ** math.floor(math.log10(raw))
    for mult in (1.0, 2.0, 5.0, 10.0):
        if raw <= mult * mag:
            step = mult * mag
            break
    first = math.ceil(lo / step) * step
    ticks = []
    t = first
    while t <= hi + 1e-12 * span:
        ticks.append(round(t, 12))
        t += step
    return ticks


def _fmt(v: float) -> str:
    if v == 0:
        return "0"
    if abs(v) >= 1000 or abs(v) < 0.01:
        return f"{v:.1e}"
    return f"{v:.3g}"


def lerp_color(c1: tuple[int, int, int], c2: tuple[int, int, int], t: float) -> str:
    t = max(0.0, min(1.0, t))
    r = int(c1[0] + (c2[0] - c1[0]) * t)
    g = int(c1[1] + (c2[1] - c1[1]) * t)
    b = int(c1[2] + (c2[2] - c1[2]) * t)
    return f"#{r:02x}{g:02x}{b:02x}"


def diverging(v: float, lo: float, hi: float) -> str:
    """Blue (lo) -> white (0) -> red (hi)."""
    if v < 0:
        return lerp_color((255, 255, 255), (31, 78, 168), min(1.0, v / lo) if lo < 0 else 0.0)
    return lerp_color((255, 255, 255), (178, 24, 43), min(1.0, v / hi) if hi > 0 else 0.0)


def sequential(v: float, lo: float, hi: float) -> str:
    t = 0.0 if hi <= lo else (v - lo) / (hi - lo)
    return lerp_color((247, 251, 255), (165, 15, 21), t)


@dataclass
class Series:
    label: str
    xs: list[float]
    ys: list[float]
    color: str = "#2980b9"
    width: float = 1.6


@dataclass
class Marker:
    x: float
    y: float
    label: str
    color: str = "#111"


def line_chart(
    series: list[Series],
    *,
    title: str = "",
    xlabel: str = "",
    ylabel: str = "",
    width: int = 680,
    height: int = 300,
    shade_x: tuple[float, float] | None = None,
    markers: list[Marker] = [],
    y_zero_line: bool = False,
) -> str:
    ml, mr, mt, mb = 62, 16, 30, 44
    pw, ph = width - ml - mr, height - mt - mb
    xs_all = [x for s in series for x in s.xs]
    ys_all = [y for s in series for y in s.ys]
    if not xs_all:
        return "<svg/>"
    x_lo, x_hi = min(xs_all), max(xs_all)
    y_lo, y_hi = min(ys_all), max(ys_all)
    if y_zero_line:
        y_lo, y_hi = min(y_lo, 0.0), max(y_hi, 0.0)
    if x_hi == x_lo:
        x_hi = x_lo + 1
    if y_hi == y_lo:
        y_hi = y_lo + 1
    pad = 0.06 * (y_hi - y_lo)
    y_lo, y_hi = y_lo - pad, y_hi + pad

    def px(x: float) -> float:
        return ml + (x - x_lo) / (x_hi - x_lo) * pw

    def py(y: float) -> float:
        return mt + (1.0 - (y - y_lo) / (y_hi - y_lo)) * ph

    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {width} {height}' "
        f"width='{width}' height='{height}'>",
        f"<rect x='0' y='0' width='{width}' height='{height}' fill='white'/>",
        f"<text x='{width/2}' y='18' text-anchor='middle' font-size='13' font-weight='bold' "
        f"{FONT}>{escape(title)}</text>",
    ]
    if shade_x:
        sx0, sx1 = max(shade_x[0], x_lo), min(shade_x[1], x_hi)
        if sx1 > sx0:
            parts.append(
                f"<rect x='{px(sx0):.1f}' y='{mt}' width='{px(sx1)-px(sx0):.1f}' "
                f"height='{ph}' fill='#f0f6e8'/>"
            )
    for t in nice_ticks(x_lo, x_hi):
        parts.append(
            f"<line x1='{px(t):.1f}' y1='{mt}' x2='{px(t):.1f}' y2='{mt+ph}' "
            f"stroke='#eee'/><text x='{px(t):.1f}' y='{mt+ph+16}' text-anchor='middle' "
            f"font-size='10' {FONT}>{_fmt(t)}</text>"
        )
    for t in nice_ticks(y_lo, y_hi):
        parts.append(
            f"<line x1='{ml}' y1='{py(t):.1f}' x2='{ml+pw}' y2='{py(t):.1f}' "
            f"stroke='#eee'/><text x='{ml-6}' y='{py(t)+3:.1f}' text-anchor='end' "
            f"font-size='10' {FONT}>{_fmt(t)}</text>"
        )
    if y_zero_line and y_lo < 0 < y_hi:
        parts.append(f"<line x1='{ml}' y1='{py(0):.1f}' x2='{ml+pw}' y2='{py(0):.1f}' stroke='#999' stroke-dasharray='4 3'/>")
    parts.append(f"<rect x='{ml}' y='{mt}' width='{pw}' height='{ph}' fill='none' stroke='#444'/>")

    for s in series:
        pts = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in zip(s.xs, s.ys))
        parts.append(
            f"<polyline points='{pts}' fill='none' stroke='{s.color}' stroke-width='{s.width}'/>"
        )
    for m in markers:
        parts.append(
            f"<circle cx='{px(m.x):.1f}' cy='{py(m.y):.1f}' r='4' fill='{m.color}'/>"
            f"<text x='{px(m.x)+6:.1f}' y='{py(m.y)-6:.1f}' font-size='10' {FONT} "
            f"fill='{m.color}'>{escape(m.label)}</text>"
        )
    # legend
    if len(series) > 1:
        lx = ml + 10
        for idx, s in enumerate(series):
            ly = mt + 12 + idx * 15
            parts.append(
                f"<line x1='{lx}' y1='{ly}' x2='{lx+18}' y2='{ly}' stroke='{s.color}' "
                f"stroke-width='2'/><text x='{lx+24}' y='{ly+4}' font-size='10' "
                f"{FONT}>{escape(s.label)}</text>"
            )
    parts.append(
        f"<text x='{ml+pw/2}' y='{height-8}' text-anchor='middle' font-size='11' {FONT}>"
        f"{escape(xlabel)}</text>"
        f"<text x='14' y='{mt+ph/2}' text-anchor='middle' font-size='11' {FONT} "
        f"transform='rotate(-90 14 {mt+ph/2})'>{escape(ylabel)}</text></svg>"
    )
    return "".join(parts)


def bar_chart(
    labels: list[str],
    values: list[float],
    *,
    title: str = "",
    ylabel: str = "",
    color: str = "#2980b9",
    width: int = 560,
    height: int = 280,
    annotate: bool = True,
) -> str:
    ml, mr, mt, mb = 70, 16, 30, 56
    pw, ph = width - ml - mr, height - mt - mb
    v_hi = max(values) if values else 1.0
    if v_hi <= 0:
        v_hi = 1.0
    n = max(len(values), 1)
    bw = pw / n * 0.62
    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {width} {height}' "
        f"width='{width}' height='{height}'>",
        f"<rect width='{width}' height='{height}' fill='white'/>",
        f"<text x='{width/2}' y='18' text-anchor='middle' font-size='13' font-weight='bold' "
        f"{FONT}>{escape(title)}</text>",
    ]
    for t in nice_ticks(0, v_hi):
        y = mt + (1 - t / (v_hi * 1.06)) * ph
        parts.append(
            f"<line x1='{ml}' y1='{y:.1f}' x2='{ml+pw}' y2='{y:.1f}' stroke='#eee'/>"
            f"<text x='{ml-6}' y='{y+3:.1f}' text-anchor='end' font-size='10' {FONT}>{_fmt(t)}</text>"
        )
    for idx, (lab, v) in enumerate(zip(labels, values)):
        cx = ml + pw * (idx + 0.5) / n
        bh = (v / (v_hi * 1.06)) * ph
        parts.append(
            f"<rect x='{cx-bw/2:.1f}' y='{mt+ph-bh:.1f}' width='{bw:.1f}' height='{bh:.1f}' "
            f"fill='{color}'/>"
            f"<text x='{cx:.1f}' y='{mt+ph+14}' text-anchor='middle' font-size='10' "
            f"{FONT}>{escape(lab)}</text>"
        )
        if annotate:
            parts.append(
                f"<text x='{cx:.1f}' y='{mt+ph-bh-5:.1f}' text-anchor='middle' font-size='10' "
                f"font-weight='bold' {FONT}>{_fmt(v)}</text>"
            )
    parts.append(
        f"<rect x='{ml}' y='{mt}' width='{pw}' height='{ph}' fill='none' stroke='#444'/>"
        f"<text x='14' y='{mt+ph/2}' text-anchor='middle' font-size='11' {FONT} "
        f"transform='rotate(-90 14 {mt+ph/2})'>{escape(ylabel)}</text></svg>"
    )
    return "".join(parts)


@dataclass
class RectMap:
    """Rect map in data coordinates (x right, y up) rendered to SVG."""

    x0: float
    x1: float
    y0: float
    y1: float
    title: str = ""
    xlabel: str = ""
    ylabel: str = ""
    width: int = 760
    rects: list[tuple[float, float, float, float, str]] = field(default_factory=list)
    outlines: list[tuple[float, float, float, float, str]] = field(default_factory=list)
    markers: list[Marker] = field(default_factory=list)
    legend: list[tuple[str, str]] = field(default_factory=list)

    def add_rect(self, x: float, y: float, w: float, h: float, fill: str) -> None:
        self.rects.append((x, y, w, h, fill))

    def render(self) -> str:
        ml, mr, mt, mb = 56, 14, 30, 60 if self.legend else 44
        pw = self.width - ml - mr
        aspect = (self.y1 - self.y0) / (self.x1 - self.x0)
        ph = max(60, int(pw * aspect))
        height = ph + mt + mb

        def px(x: float) -> float:
            return ml + (x - self.x0) / (self.x1 - self.x0) * pw

        def py(y: float) -> float:
            return mt + (1.0 - (y - self.y0) / (self.y1 - self.y0)) * ph

        parts = [
            f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {self.width} {height}' "
            f"width='{self.width}' height='{height}'>",
            f"<rect width='{self.width}' height='{height}' fill='white'/>",
            f"<text x='{self.width/2}' y='18' text-anchor='middle' font-size='13' "
            f"font-weight='bold' {FONT}>{escape(self.title)}</text>",
        ]
        for x, y, w, h, fill in self.rects:
            parts.append(
                f"<rect x='{px(x):.2f}' y='{py(y+h):.2f}' width='{max(px(x+w)-px(x),0.4):.2f}' "
                f"height='{max(py(y)-py(y+h),0.4):.2f}' fill='{fill}'/>"
            )
        for x, y, w, h, stroke in self.outlines:
            parts.append(
                f"<rect x='{px(x):.2f}' y='{py(y+h):.2f}' width='{px(x+w)-px(x):.2f}' "
                f"height='{py(y)-py(y+h):.2f}' fill='none' stroke='{stroke}' stroke-width='1.4'/>"
            )
        for m in self.markers:
            parts.append(
                f"<line x1='{px(m.x):.1f}' y1='{py(self.y0):.1f}' x2='{px(m.x):.1f}' "
                f"y2='{py(self.y0)+8:.1f}' stroke='{m.color}' stroke-width='2'/>"
                f"<circle cx='{px(m.x):.1f}' cy='{py(m.y):.1f}' r='3.5' fill='{m.color}'/>"
                f"<text x='{px(m.x):.1f}' y='{py(m.y)-7:.1f}' text-anchor='middle' "
                f"font-size='10' font-weight='bold' fill='{m.color}' {FONT}>{escape(m.label)}</text>"
            )
        for t in nice_ticks(self.x0, self.x1, 8):
            parts.append(
                f"<text x='{px(t):.1f}' y='{mt+ph+14}' text-anchor='middle' font-size='10' "
                f"{FONT}>{_fmt(t)}</text>"
            )
        for t in nice_ticks(self.y0, self.y1, 4):
            parts.append(
                f"<text x='{ml-6}' y='{py(t)+3:.1f}' text-anchor='end' font-size='10' "
                f"{FONT}>{_fmt(t)}</text>"
            )
        parts.append(
            f"<rect x='{ml}' y='{mt}' width='{pw}' height='{ph}' fill='none' stroke='#444'/>"
            f"<text x='{ml+pw/2}' y='{mt+ph+30}' text-anchor='middle' font-size='11' "
            f"{FONT}>{escape(self.xlabel)}</text>"
            f"<text x='14' y='{mt+ph/2}' text-anchor='middle' font-size='11' {FONT} "
            f"transform='rotate(-90 14 {mt+ph/2})'>{escape(self.ylabel)}</text>"
        )
        if self.legend:
            lx = ml
            ly = mt + ph + 44
            for lab, color in self.legend:
                parts.append(
                    f"<rect x='{lx}' y='{ly-9}' width='12' height='12' fill='{color}'/>"
                    f"<text x='{lx+16}' y='{ly+1}' font-size='10' {FONT}>{escape(lab)}</text>"
                )
                lx += 16 + 7 * len(lab) + 26
        parts.append("</svg>")
        return "".join(parts)
