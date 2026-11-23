#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Render the first-party HEADUNIT splash artwork. No live display or boot changes.

Requires Python, Pillow, NumPy and ffmpeg with libx264. SVG polygon geometry is
the shared source for every export. Offline motion time has no readiness meaning.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'assets/branding/headunit'
DURATION = 4.8
FPS = 30
SIZES = {'main': (1280, 720), 'cluster': (1920, 1080), 'wide': (1920, 720)}
ICE = np.array([101, 208, 255], dtype=np.float32)


def ease(t, start, end):
    u = np.clip((t - start) / (end - start), 0, 1)
    return u * u * (3 - 2 * u)


def polygons():
    document = ET.parse(ART / 'logo.svg')
    return [(p.attrib['id'], [tuple(map(float, v.split(',')))
                             for v in p.attrib['points'].split()])
            for p in document.findall('.//{http://www.w3.org/2000/svg}polygon')]


def trace(draw, points, amount, width):
    """Follow a polygon's real edges by arc length, without revealing a box."""
    segments = list(zip(points, points[1:] + points[:1]))
    budget = sum(math.dist(a, b) for a, b in segments) * amount
    for a, b in segments:
        length = math.dist(a, b)
        if budget <= 0:
            break
        fraction = min(1, budget / length)
        end = (a[0] + (b[0] - a[0]) * fraction,
               a[1] + (b[1] - a[1]) * fraction)
        draw.line([a, end], fill=255, width=width)
        budget -= length


class Artwork:
    def __init__(self, size):
        self.width, self.height = size
        # Identical visual scale relative to height on both physical panels.
        self.scale = self.height * 0.35 / 304
        self.margin = round(48 * self.height / 720)
        self.tile = (round(430 * self.scale) + self.margin * 2,
                     round(304 * self.scale) + self.margin * 2)
        self.origin = ((self.width - self.tile[0]) // 2,
                       (self.height - self.tile[1]) // 2)
        self.shapes = []
        aa = 3
        for name, points in polygons():
            mapped = [(x * self.scale + self.margin,
                       y * self.scale + self.margin) for x, y in points]
            mask = Image.new('L', (self.tile[0] * aa, self.tile[1] * aa))
            ImageDraw.Draw(mask).polygon([(x * aa, y * aa) for x, y in mapped], fill=255)
            mask = mask.resize(self.tile, Image.Resampling.LANCZOS)
            self.shapes.append((name, mapped, np.asarray(mask, dtype=np.float32) / 255))
        y, x = np.mgrid[:self.height, :self.width].astype(np.float32)
        radius = ((x - self.width / 2) / (self.height * 0.8)) ** 2 + \
                 ((y - self.height * 0.48) / (self.height * 0.65)) ** 2
        halo = np.exp(-radius * 2)[..., None]
        self.background = np.array([3, 5, 8]) + halo * np.array([4, 5, 7])
        self.background = self.background.astype(np.float32)
        oy, ox = self.origin[1], self.origin[0]
        self.base_tile = self.background[oy:oy + self.tile[1], ox:ox + self.tile[0]].copy()
        self.y, self.x = np.mgrid[:self.tile[1], :self.tile[0]].astype(np.float32)
        vertical = np.clip((self.y - self.margin) / (304 * self.scale), 0, 1)
        metal = 202 - vertical * 53 + 18 * np.cos(vertical * math.pi * 2.6)
        self.metal = np.stack([metal * 0.94, metal, metal * 1.04], axis=-1)

    def frame(self, t):
        fill = np.zeros(self.tile[::-1], dtype=np.float32)
        edge = Image.new('L', self.tile)
        edge_draw = ImageDraw.Draw(edge)
        for name, points, mask in self.shapes:
            delay = 0.13 if name == 'display-link' else 0
            amount = ease(t, 0.22 + delay, 1.3 + delay)
            trace(edge_draw, points, amount, max(1, round(self.scale * 1.1)))
            reveal = np.clip((t - 0.70 - delay) / 0.86, 0, 1)
            front = self.margin + reveal * 260 * self.scale
            scan = np.clip((front - self.y) / (20 * self.scale), 0, 1)
            fill += mask * scan * ease(t, 0.76 + delay, 1.56 + delay)
        fill = np.clip(fill, 0, 1)
        edge_strength = ease(t, 0.20, 0.40) * (1 - ease(t, 1.45, 2.25))
        outline = np.asarray(edge, dtype=np.float32) / 255 * edge_strength
        blur = max(1, self.scale * 9)
        glow = np.asarray(edge.filter(ImageFilter.GaussianBlur(blur)), dtype=np.float32) / 255
        bloom = np.asarray(edge.filter(ImageFilter.GaussianBlur(blur * 2.8)), dtype=np.float32) / 255
        tile = self.base_tile.copy()
        tile += (glow[..., None] * 1.8 + bloom[..., None] * 1.6) * ICE * edge_strength
        # One oblique reflection; it finishes before the final still hold.
        sweep_u = (t - 1.22) / 1.32
        sweep_x = self.margin - 110 * self.scale + sweep_u * 670 * self.scale
        band = np.exp(-((self.x - sweep_x + (self.y - self.margin) * 0.27) /
                        (24 * self.scale)) ** 2)
        band *= ease(t, 1.20, 1.48) * (1 - ease(t, 2.30, 2.63))
        metal = self.metal + band[..., None] * np.array([51, 59, 67])
        tile = tile * (1 - fill[..., None]) + metal * fill[..., None]
        tile += outline[..., None] * ICE * 0.85
        frame = self.background.copy()
        ox, oy = self.origin
        frame[oy:oy + self.tile[1], ox:ox + self.tile[0]] = tile
        frame *= ease(t, 0, 0.20)
        return Image.fromarray(np.clip(frame, 0, 255).astype(np.uint8))


def render(role, destination):
    width, height = SIZES[role]
    artwork = Artwork((width, height))
    movie = destination / f'headunit-{role}.mp4'
    encoder = shutil.which('ffmpeg')
    if not encoder:
        raise RuntimeError('ffmpeg is required')
    command = [encoder, '-hide_banner', '-loglevel', 'error', '-y',
               '-f', 'rawvideo', '-pixel_format', 'rgb24', '-video_size', f'{width}x{height}',
               '-framerate', str(FPS), '-i', 'pipe:0', '-an', '-c:v', 'libx264',
               '-preset', 'slow', '-crf', '17', '-pix_fmt', 'yuv420p',
               '-movflags', '+faststart', str(movie)]
    with subprocess.Popen(command, stdin=subprocess.PIPE) as process:
        try:
            for frame in range(round(DURATION * FPS)):
                process.stdin.write(artwork.frame(frame / FPS).tobytes())
            process.stdin.close()
            if process.wait() != 0:
                raise RuntimeError('ffmpeg export failed')
        except BaseException:
            process.kill()
            process.wait()
            movie.unlink(missing_ok=True)
            raise
    artwork.frame(DURATION).save(destination / f'headunit-{role}-hold.png')
    print(f'{role}: {width} × {height}, {FPS} fps, {DURATION}s', flush=True)


def contact_sheet(destination):
    art = Artwork(SIZES['main'])
    times = [0.50, 0.90, 1.25, 1.70, 2.20, 3.00]
    sheet = Image.new('RGB', (960, 590), '#10151d')
    draw = ImageDraw.Draw(sheet)
    for i, t in enumerate(times):
        thumb = art.frame(t).resize((480, 270), Image.Resampling.LANCZOS)
        x, y = (i % 2) * 480, (i // 2) * 196
        # Crop background-only margins, preserving logo and negative space.
        thumb = thumb.crop((0, 45, 480, 225))
        sheet.paste(thumb, (x, y))
        draw.text((x + 16, y + 180), f'{t:.2f}s', fill='#94a4b8')
    sheet.save(destination / 'storyboard.jpg', quality=95)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ART / 'exports')
    parser.add_argument('--role', choices=['all', *SIZES], default='all')
    parser.add_argument('--update-startup-geometry', action='store_true', help='also update the sealed live-vector geometry from logo.svg')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for role in SIZES if args.role == 'all' else [args.role]:
        render(role, args.output)
    contact_sheet(args.output)
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(args.output.glob('*')) if p.suffix in ('.mp4', '.png', '.jpg')}
    receipt = {'duration_seconds': DURATION, 'fps': FPS, 'audio': False,
               'sizes': SIZES, 'geometry_sha256': hashlib.sha256((ART / 'logo.svg').read_bytes()).hexdigest(),
               'renderer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'files_sha256': files, 'evidence': 'Offline asset rendering; no CM5 boot qualification.'}
    (args.output / 'manifest.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if args.update_startup_geometry:
        module = ROOT / 'tools/phone_icons.py'
        marker = '# Generated geometry from assets/branding/headunit/logo.svg; no runtime file input.\n'
        geometry = tuple((name, tuple(points)) for name, points in polygons())
        result = module.read_text().split(marker)[0].rstrip() + '\n\n' + marker + '_STARTUP_POLYGONS = ' + repr(geometry) + '\n'
        if len(result.encode()) > 131072:
            raise ValueError('Artwork module exceeds the retained module limit')
        module.write_text(result)


if __name__ == '__main__':
    main()
