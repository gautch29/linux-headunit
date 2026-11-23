# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Self-contained original main-screen sequence: no GTK, sockets or power control.

The native UI can draw frame().svg into the existing page renderer and retire the
component on readiness/withdrawal. Timings are cosmetic, never receipts.
"""
from dataclasses import dataclass
import math
import time
import sys

VARIANTS = ('off', 'brand', 'minimal')
# The acquisition domain of headunit::Clock (libc++ on macOS, libstdc++ on Linux).
ACQUISITION_CLOCK = time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin' else time.CLOCK_MONOTONIC
POLYGONS = (
    ((24, 76), (239, 76), (239, 184), (221, 166), (221, 94), (42, 94), (42, 194), (217, 194), (235, 212), (24, 212)),
    ((194, 143), (222, 143), (279, 200), (251, 200)),
    ((264, 104), (406, 104), (406, 228), (290, 228), (272, 210), (388, 210), (388, 122), (282, 122), (282, 176), (264, 158)),
)


def monotonic():
    """Same actual host clock used by C++/dated IPC, including on macOS."""
    return time.clock_gettime(ACQUISITION_CLOCK)


def ease(value):
    value = max(0., min(1., value))
    return value * value * (3 - 2 * value)


@dataclass(frozen=True)
class Frame:
    phase: str = 'inactive'
    variant: str = 'off'
    opacity: float = 0.
    reveal: float = 0.

    def svg(self, width, height):
        """Transparent bounded vector component, original HEADUNIT geometry.

        No scene cover, background, percentage, audio or copyrighted brand.
        A renderer can cache polygon paths and use opacity/reveal directly.
        """
        if type(width) is not int or type(height) is not int or not 240 <= width <= 4096 or not 240 <= height <= 2160:
            raise ValueError('Sequence allocation outside supported range')
        if self.phase == 'inactive' or self.opacity <= 0:
            return ''
        scale = height * .35 / 304
        x = (width - 430 * scale) / 2
        y = (height - 304 * scale) / 2
        shapes = []
        if self.variant == 'brand':
            for points in POLYGONS:
                coordinates = ' '.join(f'{px:g},{py:g}' for px, py in points)
                shapes.append(f'<polygon points="{coordinates}" fill="#cce6ff" fill-opacity="{self.reveal:.6f}" stroke="#66d1ff" stroke-opacity="{1-self.reveal:.6f}" stroke-width="2"/>')
        else:
            shapes.append(f'<path d="M {215-200*self.reveal:g} 152 H {215+200*self.reveal:g}" stroke="#66d1ff" stroke-width="3"/>')
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
                f'<g opacity="{self.opacity:.6f}" transform="translate({x:g} {y:g}) scale({scale:g})">'
                + ''.join(shapes) + '</g></svg>')


@dataclass(frozen=True)
class IgnitionObservation:
    on: bool | None
    acquired_at: float | None
    provenance: str
    source_generation: str


class GreetingSequence:
    """One nonblocking attempt per explicit UI event; the owner admits boot/wake.

    Call begin only for actual admitted wake or a labelled user preview. A
    phone connection is not wake; no automatic replay or periodic renewal.
    This component cannot operate ignition, cutoff or receiver services.
    """
    def __init__(self, clock=monotonic):
        self.clock = clock
        self._ignition = None
        self._ignition_date = None
        self._ignition_generation = None
        self.retire()

    def observe_ignition(self, reading, variant):
        """Future power-provider adapter: only fresh observed edges animate.

        Initial Off, missing/stale state, a new source epoch or backwards
        acquisition creates no inferred edge. Original acquisition dates and
        provenance remain in the observation; animation cannot confirm power.
        """
        if variant not in VARIANTS:
            raise ValueError("Invalid sequence variant")
        now = self.clock()
        if (not isinstance(reading, IgnitionObservation) or type(reading.on) is not bool or
                type(reading.acquired_at) not in (int, float) or
                type(now) not in (int, float) or not math.isfinite(now) or
                not math.isfinite(reading.acquired_at) or not 0 <= now - reading.acquired_at <= .5 or
                type(reading.provenance) is not str or not 1 <= len(reading.provenance) <= 128 or
                type(reading.source_generation) is not str or not 1 <= len(reading.source_generation) <= 128):
            self._ignition = self._ignition_date = self._ignition_generation = None
            self.retire()
            return False
        if reading.source_generation != self._ignition_generation:
            self.retire()
            self._ignition = None
            self._ignition_date = None
        if self._ignition_date is not None and reading.acquired_at <= self._ignition_date:
            return False
        previous = self._ignition
        self._ignition = reading.on
        self._ignition_date = reading.acquired_at
        self._ignition_generation = reading.source_generation
        if previous is None or previous == reading.on:
            return False
        self.begin("startup" if reading.on else "goodbye", variant)
        # A delayed delivery cannot extend the original event timeline.
        self.started = self.last = reading.acquired_at
        return True

    def retire(self):
        self.phase = 'inactive'
        self.started = self.last = None
        self.variant = 'off'

    def begin(self, phase, variant):
        if phase not in ('startup', 'goodbye') or variant not in VARIANTS:
            raise ValueError('Invalid sequence phase or variant')
        at = self.clock()
        if type(at) not in (int, float) or not math.isfinite(at) or at < 0:
            self.retire()
            return
        self.phase = phase if variant != 'off' else 'inactive'
        self.variant = variant
        self.started = self.last = at

    def frame(self):
        if self.phase == 'inactive':
            return Frame()
        now = self.clock()
        if type(now) not in (int, float) or not math.isfinite(now) or now < self.last:
            self.retire()
            return Frame()
        self.last = now
        elapsed = now - self.started
        if elapsed >= (1.8 if self.phase == 'startup' else .9):
            self.retire()
            return Frame()
        if self.phase == 'startup':
            reveal = ease(elapsed / .65)
            opacity = ease(elapsed / .12) * (1 - ease((elapsed - 1.1) / .7))
        else:
            reveal = 1.
            opacity = ease(elapsed / .1) * (1 - ease((elapsed - .25) / .65))
        return Frame(self.phase, self.variant, opacity, reveal)
