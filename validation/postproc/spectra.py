"""Spectral analysis of force probes: DFT of Cl(t) -> Strouhal number.

The solver uses a CFL-adaptive dt, so samples are first resampled onto a
uniform time grid (linear interpolation), Hann-windowed, then transformed with
a direct DFT (sample counts are small; O(N^2) is fine and dependency-free).
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass
class StrouhalResult:
    strouhal: float | None  # None when no dominant peak (steady wake)
    frequency: float | None
    peak_magnitude: float
    prominent: bool
    resolution_st: float    # frequency-bin width expressed as a Strouhal number
    freqs: list[float]      # one-sided spectrum (for plotting)
    mags: list[float]


def resample_uniform(t: list[float], y: list[float], n: int | None = None) -> tuple[float, list[float]]:
    """Linear-interpolate (t, y) onto n uniform samples over [t0, tN]."""
    if len(t) < 4:
        raise ValueError("resample_uniform: need at least 4 samples")
    if n is None:
        n = len(t)
    t0, t1 = t[0], t[-1]
    dt = (t1 - t0) / (n - 1)
    out = []
    j = 0
    for i in range(n):
        ti = t0 + i * dt
        while j + 1 < len(t) - 1 and t[j + 1] < ti:
            j += 1
        ta, tb = t[j], t[j + 1]
        ya, yb = y[j], y[j + 1]
        w = 0.0 if tb == ta else (ti - ta) / (tb - ta)
        out.append(ya + w * (yb - ya))
    return dt, out


def dft_onesided(samples: list[float], dt: float) -> tuple[list[float], list[float]]:
    """Hann-windowed one-sided DFT magnitudes; DC removed beforehand."""
    n = len(samples)
    mean = sum(samples) / n
    w = [0.5 * (1.0 - math.cos(2.0 * math.pi * i / (n - 1))) for i in range(n)]
    x = [(samples[i] - mean) * w[i] for i in range(n)]
    freqs, mags = [], []
    for k in range(n // 2 + 1):
        re = im = 0.0
        for i in range(n):
            ang = -2.0 * math.pi * k * i / n
            re += x[i] * math.cos(ang)
            im += x[i] * math.sin(ang)
        freqs.append(k / (n * dt))
        mags.append(math.hypot(re, im) * 2.0 / n)
    return freqs, mags


def strouhal_analysis(
    t: list[float], cl: list[float], h: float = 1.0, velocity: float = 1.0
) -> StrouhalResult:
    """St = f_peak * h / U from the Cl probe. A peak counts as dominant only if
    it clears mean + 4*sigma of the rest of the spectrum — otherwise the wake is
    reported as steady (no fabricated shedding frequency)."""
    dt, samples = resample_uniform(t, cl)

    # Linear detrend: a slow convergence drift is a ramp whose 1/f spectrum
    # would otherwise masquerade as a low-frequency "shedding" peak.
    n = len(samples)
    mean0 = sum(samples) / n
    tbar = (n - 1) / 2.0
    denom = sum((i - tbar) ** 2 for i in range(n))
    slope = sum((i - tbar) * samples[i] for i in range(n)) / denom if denom else 0.0
    samples = [samples[i] - (mean0 + slope * (i - tbar)) for i in range(n)]

    freqs, mags = dft_onesided(samples, dt)
    st_scale = h / velocity
    resolution = (freqs[1] - freqs[0]) * st_scale if len(freqs) > 1 else 0.0

    # skip DC and the first bin (windowing leakage)
    candidates = list(range(2, len(mags)))
    if not candidates:
        return StrouhalResult(None, None, 0.0, False, resolution, freqs, mags)
    k_peak = max(candidates, key=lambda k: mags[k])
    others = [mags[k] for k in candidates if abs(k - k_peak) > 1]
    mean_o = sum(others) / len(others) if others else 0.0
    var_o = sum((v - mean_o) ** 2 for v in others) / len(others) if others else 0.0
    noise_floor = 1e-9 * (1.0 + abs(mean0))  # numerical residue is not a peak
    prominent = mags[k_peak] > mean_o + 4.0 * math.sqrt(var_o) and mags[k_peak] > noise_floor

    if not prominent:
        return StrouhalResult(None, None, mags[k_peak], False, resolution, freqs, mags)
    f = freqs[k_peak]
    return StrouhalResult(f * st_scale, f, mags[k_peak], True, resolution, freqs, mags)
