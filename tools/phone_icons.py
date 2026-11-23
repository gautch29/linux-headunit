# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Original artwork for native controls and startup."""
import math
import gi

gi.require_version('GdkPixbuf', '2.0')
gi.require_version('Gdk', '4.0')
gi.require_version('Gtk', '4.0')
from gi.repository import Gdk, GdkPixbuf, Gtk

_PATHS = {
    'phone': '<rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 5h4M10 19h4"/>',
    'music': '<path d="M9 17V5l11-2v12M9 8l11-2"/><ellipse cx="6" cy="18" rx="3" ry="2"/><ellipse cx="17" cy="16" rx="3" ry="2"/>',
    'cluster': '<path d="M3 17a9 9 0 1 1 18 0M6 18h12M12 13l4-5"/><circle cx="12" cy="13" r="1.2" fill="currentColor"/>',
    'camera': '<rect x="3" y="6" width="18" height="15" rx="3"/><path d="m8 6 2-3h4l2 3"/><circle cx="12" cy="13" r="4"/>',
    'settings': '<path d="M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1"/><circle cx="15" cy="6" r="2"/><circle cx="9" cy="12" r="2"/><circle cx="17" cy="18" r="2"/>',
    'smooth': '<path d="M3 15c3-6 6-6 9 0s6 6 9 0"/>',
    'balanced': '<path d="M12 4v16M5 8h14M7 8l-3 6h6ZM17 8l-3 6h6Z"/>',
    'fast': '<path d="M13 3 5 13h6l-1 8 8-10h-6Z"/>',
    'previous': '<path d="M5 5v14"/><path d="M19 5l-10 7 10 7Z" fill="currentColor" stroke="none"/>',
    'play': '<path d="m8 4 12 8-12 8Z" fill="currentColor" stroke="none"/>',
    'pause': '<rect x="6.5" y="5" width="3.5" height="14" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="5" width="3.5" height="14" rx="1" fill="currentColor" stroke="none"/>',
    'next': '<path d="M19 5v14"/><path d="M5 5l10 7-10 7Z" fill="currentColor" stroke="none"/>',
    'map': '<path d="m3 5 6-2 6 2 6-2v16l-6 2-6-2-6 2ZM9 3v16M15 5v16"/>',
    'vehicle': '<path d="m5 9 2-5h10l2 5M3 10l2-1h14l2 1v8H3ZM5 18v3M19 18v3M6 13h2M16 13h2"/>',
    'trip': '<circle cx="12" cy="12" r="9"/><path d="m16 8-3 5-5 3 3-5Z"/>',
    'volume_down': '<path d="M11 5 6 9H3v6h3l5 4ZM15 9a5 5 0 0 1 0 6"/>',
    'volume_up': '<path d="M11 5 6 9H3v6h3l5 4ZM15 9a5 5 0 0 1 0 6M18 5a10 10 0 0 1 0 14"/>',
    'home': '<path d="M3 11 12 4l9 7M5.5 9.5V20h13V9.5"/><path d="M10 20v-5h4v5"/>',
}
# Filled white glyphs for the Home app launchers, drawn on a 24-unit grid.
_APP_GLYPHS = {
    'maps': '<path d="M12 3.5 18.5 19 12 15.6 5.5 19Z" fill="#ffffff"/>',
    'music': '<path d="M9.5 16.5V6.2l9-1.9v10.2" stroke="#ffffff" stroke-width="2" fill="none" '
             'stroke-linecap="round" stroke-linejoin="round"/>'
             '<ellipse cx="7.3" cy="16.8" rx="2.6" ry="2.1" fill="#ffffff"/>'
             '<ellipse cx="16.3" cy="14.8" rx="2.6" ry="2.1" fill="#ffffff"/>',
    'phone': '<path d="M7.2 3.6c.6-.4 1.4-.3 1.8.3l1.9 2.8c.4.6.3 1.4-.2 1.9l-1.3 1.2c.9 2 2.6 3.7 4.6 '
             '4.6l1.2-1.3c.5-.5 1.3-.6 1.9-.2l2.8 1.9c.6.4.7 1.2.3 1.8l-1.1 1.7c-.6.9-1.7 1.4-2.8 '
             '1.1C10 18.1 5.9 14 4.6 8.7c-.3-1.1.2-2.2 1.1-2.8Z" fill="#ffffff"/>',
}
# Top and bottom gradient stops, in the spirit of the system app icons.
_APP_COLOURS = {'maps': ('#5ac8fa', '#007aff'), 'music': ('#ff6482', '#fa2d48'),
                'phone': ('#65e07a', '#29c043')}
_APP_CACHE = {}
_CACHE = {}

# Dock symbol colours on the dark glass bar: unselected, selected.
NAV_COLOURS = {False: '#f5f5f7', True: '#0a84ff'}


def navigation_icon(name, selected=False):
    return icon(name, 40, NAV_COLOURS[selected])


def icon(name, size=24, color='#e0e3eb'):
    """An SF Symbol in `color` when the private input exists, else native vector."""
    key = (name, size, color)
    if key not in _CACHE:
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
               f'color="{color}" fill="none" stroke="{color}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">'
               + _PATHS[name] + '</svg>')
        loader = GdkPixbuf.PixbufLoader.new_with_type('svg')
        # Keep edges crisp on HiDPI displays too; the widget uses logical pixels.
        loader.set_size(size * 2, size * 2)
        loader.write(svg.encode())
        loader.close()
        _CACHE[key] = Gdk.Texture.new_for_pixbuf(loader.get_pixbuf())
    image = Gtk.Image.new_from_paintable(_CACHE[key])
    image.set_pixel_size(size)
    return image


def app_icon(name, size=76):
    """Create a launcher tile from the project-owned glyph and gradient."""
    key = (name, size)
    if key not in _APP_CACHE:
        top, bottom = _APP_COLOURS[name]
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">'
               '<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">'
               f'<stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/>'
               '</linearGradient></defs>'
               '<rect width="24" height="24" rx="5.4" fill="url(#g)"/>'
               '<g transform="translate(3.6 3.6) scale(0.7)">' + _APP_GLYPHS[name] + '</g></svg>')
        loader = GdkPixbuf.PixbufLoader.new_with_type('svg')
        loader.set_size(size * 2, size * 2)
        loader.write(svg.encode())
        loader.close()
        _APP_CACHE[key] = Gdk.Texture.new_for_pixbuf(loader.get_pixbuf())
    image = Gtk.Image.new_from_paintable(_APP_CACHE[key])
    image.set_pixel_size(size)
    image.add_css_class('app-icon')
    return image


def startup_ease(t, start, end):
    value = max(0., min(1., (t - start) / (end - start)))
    return value * value * (3 - 2 * value)


def startup_trace(points, amount):
    """Bounded edge-length reveal, in the original vector coordinates."""
    segments = tuple(zip(points, points[1:] + points[:1]))
    lengths = tuple(math.dist(a, b) for a, b in segments)
    remaining = sum(lengths) * max(0., min(1., amount))
    result = [points[0]]
    for (a, b), length in zip(segments, lengths):
        if remaining <= 0:
            break
        fraction = min(1., remaining / length)
        result.append((a[0] + (b[0] - a[0]) * fraction,
                       a[1] + (b[1] - a[1]) * fraction))
        remaining -= length
    return tuple(result)


def startup_canvas(owner, output):
    """Native GSK paths/gradients; no video, Cairo frames or texture sequence.

    GTK's existing renderer owns GPU submission. Each output draws its own
    vector geometry at its allocated size; both share a monotonic timeline.
    Snapshot statistics describe scene construction, not scanout receipts.
    """
    gi.require_version('Gsk', '4.0')
    gi.require_version('Graphene', '1.0')
    from gi.repository import Gsk, Graphene

    def color(r, g, b, a=1.):
        value = Gdk.RGBA()
        value.red, value.green, value.blue, value.alpha = r / 255, g / 255, b / 255, a
        return value

    def point(x, y):
        return Graphene.Point().init(x, y)

    def rect(x, y, w, h):
        return Graphene.Rect().init(x, y, w, h)

    def stops(values):
        result = []
        for offset, rgba in values:
            stop = Gsk.ColorStop()
            stop.offset, stop.color = offset, rgba
            result.append(stop)
        return result

    def path(points, close=True):
        builder = Gsk.PathBuilder.new()
        builder.move_to(*points[0])
        for p in points[1:]:
            builder.line_to(*p)
        if close:
            builder.close()
        return builder.to_path()

    paths = tuple(path(points) for _, points in _STARTUP_POLYGONS)
    white, black = color(255, 255, 255), color(0, 0, 0)
    ice = color(101, 208, 255)
    metal = stops([(i / 16, color((202 - i / 16 * 53 + 18 * math.cos(i / 16 * math.pi * 2.6)) * .94,
                                 202 - i / 16 * 53 + 18 * math.cos(i / 16 * math.pi * 2.6),
                                 (202 - i / 16 * 53 + 18 * math.cos(i / 16 * math.pi * 2.6)) * 1.04))
                   for i in range(17)])
    halo = stops([(i / 8, color(3 + 4 * math.exp(-2 * (i / 8) ** 2),
                               5 + 5 * math.exp(-2 * (i / 8) ** 2),
                               8 + 7 * math.exp(-2 * (i / 8) ** 2))) for i in range(9)])
    stroke = Gsk.Stroke.new(1.1)
    stroke.set_line_join(Gsk.LineJoin.ROUND)
    stroke.set_line_cap(Gsk.LineCap.ROUND)
    bounds = rect(0, 0, 430, 304)

    class Canvas(Gtk.Widget):
        def __init__(self):
            super().__init__()
            self.set_hexpand(True)
            self.set_vexpand(True)
            self.set_can_target(False)
            self.elapsed = 0.
            self.tick = self.add_tick_callback(self.advance)
            self.samples, self.last_sample = [], -1.
            self.renderer = ''
            self.size = (0, 0)

        def advance(self, widget, clock):
            if owner.closed:
                self.tick = 0
                return False
            if owner.started_at is not None:
                self.elapsed = max(0., time.monotonic() - owner.started_at)
            # Fade the entire window, including its GTK background, so the
            # existing interface underneath is revealed without a black cut.
            native = self.get_native()
            if native is not None:
                native.set_opacity(1 - startup_ease(self.elapsed, owner.FADE_START_SECONDS,
                                                   owner.DURATION_SECONDS))
            self.queue_draw()
            if 2.63 <= self.elapsed < owner.FADE_START_SECONDS or self.elapsed >= owner.DURATION_SECONDS:
                self.tick = 0
                return False  # Cache the hold; resume ticks only for the exit.
            return True

        def resume(self):
            if not owner.closed and not self.tick:
                self.tick = self.add_tick_callback(self.advance)

        def do_snapshot(self, destination):
            if owner.closed:
                return
            began = time.monotonic()
            try:
                w, h = self.get_width(), self.get_height()
                if not 0 < w <= 8192 or not 0 < h <= 8192:
                    raise ValueError('Invalid startup allocation')
                native = self.get_native()
                renderer = native.get_renderer() if native is not None else None
                name = renderer.__gtype__.name if renderer is not None else ''
                if name not in ('GskGLRenderer', 'GskVulkanRenderer'):
                    raise ValueError('Startup requires the native GPU renderer')
                self.renderer, self.size = name, (w, h)
                # Submit only a complete scene. Failed construction cannot
                # leave a partial mask/transform on GTK's supplied snapshot.
                snapshot = Gtk.Snapshot.new()
                t = self.elapsed
                snapshot.append_color(black, rect(0, 0, w, h))
                snapshot.push_opacity(startup_ease(t, 0, .20))
                snapshot.append_radial_gradient(rect(0, 0, w, h), point(w / 2, h * .48),
                                                h * .8, h * .65, 0., 1., halo)
                snapshot.save()
                scale = min(h * .35 / 304, w * .8 / 430)
                snapshot.translate(point((w - 430 * scale) / 2, (h - 304 * scale) / 2))
                snapshot.scale(scale, scale)
                strength = startup_ease(t, .2, .4) * (1 - startup_ease(t, 1.45, 2.25))
                # Bound blur work to the small logo region rather than the panel.
                if strength > 0:
                    builder = Gsk.PathBuilder.new()
                    for name, points in _STARTUP_POLYGONS:
                        delay = .13 if name == 'display-link' else 0
                        contour = path(startup_trace(points,
                                                    startup_ease(t, .22 + delay, 1.3 + delay)), False)
                        builder.add_path(contour)
                    contour = builder.to_path()
                    for blur, opacity in ((25.2, .38), (9., .80), (0., .85)):
                        snapshot.push_opacity(strength * opacity)
                        if blur:
                            snapshot.push_blur(blur)
                        snapshot.append_stroke(contour, stroke, ice)
                        if blur:
                            snapshot.pop()
                        snapshot.pop()
                for (name, _), shape in zip(_STARTUP_POLYGONS, paths):
                    delay = .13 if name == 'display-link' else 0
                    alpha = startup_ease(t, .76 + delay, 1.56 + delay)
                    if alpha <= 0:
                        continue
                    snapshot.push_opacity(alpha)
                    snapshot.push_fill(shape, Gsk.FillRule.WINDING)
                    front = max(0., min(1., (t - .70 - delay) / .86)) * 260
                    snapshot.push_mask(Gsk.MaskMode.ALPHA)
                    snapshot.append_linear_gradient(bounds, point(0, front - 20), point(0, front),
                                                    stops([(0., white), (1., color(255, 255, 255, 0.))]))
                    snapshot.pop()
                    snapshot.append_linear_gradient(bounds, point(0, 0), point(0, 304), metal)
                    reflection = startup_ease(t, 1.2, 1.48) * (1 - startup_ease(t, 2.3, 2.63))
                    if reflection > 0:
                        sweep = -110 + ((t - 1.22) / 1.32) * 670
                        snapshot.append_linear_gradient(bounds, point(sweep - 72, 0), point(sweep + 72, 38.88),
                            stops([(j / 8, color(245, 251, 255,
                                  .38 * reflection * math.exp(-((j / 8 - .5) * 6) ** 2))) for j in range(9)]))
                    snapshot.pop()
                    snapshot.pop()
                    snapshot.pop()
                snapshot.restore()
                snapshot.pop()
                destination.append_node(snapshot.to_node())
                # A completed snapshot is scene construction evidence only.
                if t != self.last_sample and len(self.samples) < 512:
                    self.samples.append((t, (time.monotonic() - began) * 1000))
                    self.last_sample = t
                owner.drawn(output)
            except Exception:
                owner.render_failed()

        def retire(self):
            if self.tick:
                self.remove_tick_callback(self.tick)
                self.tick = 0
            import json
            moving = [sample for sample in self.samples if sample[0] < 2.63]
            print('Headunit splash vector output ' + json.dumps({'output': output, 'size': self.size,
                  'renderer': self.renderer, 'movingSnapshots': len(moving),
                  'movingSeconds': moving[-1][0] - moving[0][0] if len(moving) > 1 else 0.,
                  'maximumSnapshotMs': max((x[1] for x in self.samples), default=0.)},
                  separators=(',', ':')), file=sys.stderr, flush=True)

    import sys
    import time
    return Canvas()


# Generated geometry from assets/branding/headunit/logo.svg; no runtime file input.
_STARTUP_POLYGONS = (
    ('main-panel', ((24, 76), (239, 76), (239, 184), (221, 166), (221, 94), (42, 94), (42, 194), (217, 194), (235, 212), (24, 212))),
    ('display-link', ((194, 143), (222, 143), (279, 200), (251, 200))),
    ('instrument-panel', ((264, 104), (406, 104), (406, 228), (290, 228), (272, 210), (388, 210), (388, 122), (282, 122), (282, 176), (264, 158))),
)
