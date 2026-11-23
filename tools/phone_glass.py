# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native glass surfaces drawn with GTK's GSK renderer.

Surfaces sample the project-owned backdrop using textures, gradients and masks.
GTK retains composition, children and touch feedback.
"""
import math
import warnings
import gi

gi.require_version('Gsk', '4.0')
gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')
gi.require_version('Graphene', '1.0')
from gi.repository import Gdk, GLib, Graphene, Gsk, Gtk

# Selection droplet motion, seconds.
DROPLET_DURATION = 0.42


def rgba(red, green, blue, alpha):
    colour = Gdk.RGBA()
    colour.red = red
    colour.green = green
    colour.blue = blue
    colour.alpha = alpha
    return colour


def rect(x, y, width, height):
    return Graphene.Rect().init(x, y, width, height)


def shape_path(area, radius):
    """Rounded-rectangle outline as a path.

    Paths clip and stroke reliably from Python, unlike GskRoundedRect,
    whose fixed corner array does not survive the binding.
    """
    x = area.get_x()
    y = area.get_y()
    width = area.get_width()
    height = area.get_height()
    r = max(0.0, min(radius, width / 2, height / 2))
    commands = (
        f'M {x + r} {y} L {x + width - r} {y} A {r} {r} 0 0 1 {x + width} {y + r} '
        f'L {x + width} {y + height - r} A {r} {r} 0 0 1 {x + width - r} {y + height} '
        f'L {x + r} {y + height} A {r} {r} 0 0 1 {x} {y + height - r} '
        f'L {x} {y + r} A {r} {r} 0 0 1 {x + r} {y} Z'
    )
    return Gsk.Path.parse(commands)


def css_padding(widget):
    """The widget's CSS padding; snapshots start inside it."""
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', DeprecationWarning)
        border = widget.get_style_context().get_padding()
    return border.left, border.top, border.right, border.bottom


def saturation(amount, brightness):
    """Colour matrix in GSK's layout: saturate, then scale brightness."""
    s = amount
    b = brightness
    values = [
        (0.213 + 0.787 * s) * b, (0.213 - 0.213 * s) * b, (0.213 - 0.213 * s) * b, 0.0,
        (0.715 - 0.715 * s) * b, (0.715 + 0.285 * s) * b, (0.715 - 0.715 * s) * b, 0.0,
        (0.072 - 0.072 * s) * b, (0.072 - 0.072 * s) * b, (0.072 + 0.928 * s) * b, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]
    matrix = Graphene.Matrix().init_from_float(values)
    offset = Graphene.Vec4().init(0, 0, 0, 0)
    return matrix, offset


def gradient_stops(stops):
    result = []
    for offset, colour in stops:
        stop = Gsk.ColorStop()
        stop.offset = offset
        stop.color = colour
        result.append(stop)
    return result


# Overshoot of the selection spring; 1.3 settles about 6 % past the target.
OVERSHOOT = 1.3


def ease(t):
    """Ease-out with a slight overshoot before settling, like a spring."""
    t = max(0.0, min(1.0, t))
    back = OVERSHOOT + 1
    return 1 + back * (t - 1) ** 3 + OVERSHOOT * (t - 1) ** 2


# Refraction is approximated by rim bands, each showing the backdrop pulled
# further inwards towards the edge, as a rounded glass edge bends light.
REFRACTION_BANDS = 6


def corner_offset(area, dx, dy):
    """Conic-gradient offset (0 at the top, clockwise) of a corner direction."""
    angle = math.degrees(math.atan2(dx * area.get_width(), -dy * area.get_height()))
    return (angle % 360) / 360


class GlassPainter:
    """Draw one glass shape over the backdrop seen through it.

    Layers, back to front: a soft shadow; the backdrop, lightly blurred and
    saturated; rim bands that refract it inwards; a tint; light gathered
    inside the edge; and a specular rim that is brightest at the top-left
    and bottom-right corners, as if lit from above-left.
    """

    def __init__(self, owner, source, radius, tint, blur=6, magnify=1.03, rim=0.9, saturate=1.6):
        self.owner = owner
        self.source = source
        self.radius = radius
        self.tint = tint
        self.blur = blur
        self.magnify = magnify
        self.rim = rim
        self.saturate = saturate

    def backdrop(self, snapshot, area, scale, margin=0.0):
        """The backdrop behind `area`, magnified about its centre.

        `scale` is one factor or an (x, y) pair. Only `area` plus `margin` is
        drawn, so a following blur works on a small offscreen.
        """
        layer = None if self.source is None else self.source.widget
        if layer is None or not layer.get_visible():
            return
        ok, bounds = self.owner.compute_bounds(layer)
        width = layer.get_width()
        height = layer.get_height()
        if not ok or width <= 0 or height <= 0:
            return
        scale_x, scale_y = scale if isinstance(scale, tuple) else (scale, scale)
        snapshot.push_clip(rect(area.get_x() - margin, area.get_y() - margin,
                                area.get_width() + 2 * margin, area.get_height() + 2 * margin))
        centre_x = area.get_x() + area.get_width() / 2
        centre_y = area.get_y() + area.get_height() / 2
        snapshot.save()
        snapshot.translate(Graphene.Point().init(centre_x, centre_y))
        snapshot.scale(scale_x, scale_y)
        snapshot.translate(Graphene.Point().init(-centre_x - bounds.get_x(), -centre_y - bounds.get_y()))
        self.source.paint(snapshot, width, height)
        snapshot.restore()
        snapshot.pop()

    def refraction(self, snapshot, area, shape, shortest):
        """Rim bands: the nearer the edge, the further inwards the backdrop is sampled."""
        white = rgba(1, 1, 1, 1)
        bezel = min(30.0, shortest * 0.32)
        displacement = min(28.0, shortest * 0.2)
        half_width = area.get_width() / 2
        half_height = area.get_height() / 2
        for band in range(REFRACTION_BANDS):
            # Widest band first; each narrower band, nearer the edge, bends more.
            depth = bezel * (REFRACTION_BANDS - band) / REFRACTION_BANDS
            shift = displacement * ((band + 1) / REFRACTION_BANDS) ** 2
            scale = (half_width / max(1.0, half_width - shift), half_height / max(1.0, half_height - shift))
            snapshot.push_mask(Gsk.MaskMode.ALPHA)
            snapshot.push_blur(bezel / REFRACTION_BANDS)
            snapshot.append_stroke(shape, Gsk.Stroke.new(depth * 2), white)
            snapshot.pop()
            snapshot.pop()
            # Unfrosted: a glass edge shows the bent backdrop sharply.
            self.backdrop(snapshot, area, (scale[0] * self.magnify, scale[1] * self.magnify))
            snapshot.pop()

    def specular(self, snapshot, area, shape, width, alpha, blur=0.0):
        """A rim lit from the top-left: bright at that corner and the opposite one."""
        white = rgba(1, 1, 1, 1)
        top_left = corner_offset(area, -1, -1)
        bottom_right = corner_offset(area, 1, 1)
        top_right = corner_offset(area, 1, -1)
        bottom_left = corner_offset(area, -1, 1)
        stops = sorted((
            (top_right, 0.12), (bottom_left, 0.1),
            (top_left, 1.0), (bottom_right, 0.55),
            ((top_left + 0.06) % 1, 0.5), ((top_left - 0.06) % 1, 0.5),
            ((bottom_right + 0.05) % 1, 0.22), ((bottom_right - 0.05) % 1, 0.22),
        ))
        # Wrap so the gradient is continuous through offset 0.
        first, last = stops[0], stops[-1]
        span = first[0] + 1 - last[0]
        edge = last[1] + (first[1] - last[1]) * (1 - last[0]) / span if span > 0 else first[1]
        stops = [(0.0, edge)] + stops + [(1.0, edge)]
        centre = Graphene.Point().init(area.get_x() + area.get_width() / 2, area.get_y() + area.get_height() / 2)
        snapshot.push_mask(Gsk.MaskMode.ALPHA)
        if blur:
            snapshot.push_blur(blur)
        snapshot.append_stroke(shape, Gsk.Stroke.new(width), white)
        if blur:
            snapshot.pop()
        snapshot.pop()
        snapshot.append_conic_gradient(area, centre, 0, gradient_stops(
            [(offset, rgba(1, 1, 1, min(1.0, level * alpha))) for offset, level in stops]))
        snapshot.pop()

    def glow(self, snapshot, area, point, level, radius=None):
        """Light spreading through the glass from where it is touched."""
        if level <= 0.001 or point is None:
            return
        radius = self.radius if radius is None else radius
        reach = max(area.get_width(), area.get_height()) * 0.45
        centre = Graphene.Point().init(*point)
        bounds = rect(centre.x - reach, centre.y - reach, reach * 2, reach * 2)
        snapshot.push_fill(shape_path(area, radius), Gsk.FillRule.WINDING)
        snapshot.append_radial_gradient(bounds, centre, reach, reach, 0, 1, gradient_stops((
            (0.0, rgba(1, 1, 1, 0.3 * level)),
            (0.5, rgba(1, 1, 1, 0.1 * level)),
            (1.0, rgba(1, 1, 1, 0.0)),
        )))
        snapshot.pop()

    def draw(self, snapshot, area, radius=None, emphasis=1.0):
        radius = self.radius if radius is None else radius
        if area.get_width() <= 1 or area.get_height() <= 1:
            return
        shape = shape_path(area, radius)
        white = rgba(1, 1, 1, 1)
        shortest = min(area.get_width(), area.get_height())
        # Depth: a wide, soft shadow below the slab.
        drop = rect(area.get_x(), area.get_y() + 10, area.get_width(), area.get_height())
        snapshot.push_blur(20)
        snapshot.push_fill(shape_path(drop, radius), Gsk.FillRule.WINDING)
        snapshot.append_color(rgba(0, 0, 0, 0.3 * emphasis), drop)
        snapshot.pop()
        snapshot.pop()
        snapshot.push_fill(shape, Gsk.FillRule.WINDING)
        matrix, offset = saturation(self.saturate, 1.08)
        snapshot.push_color_matrix(matrix, offset)
        # Body: the backdrop stays readable through the glass, barely frosted.
        snapshot.push_blur(self.blur)
        self.backdrop(snapshot, area, self.magnify, self.blur * 2)
        snapshot.pop()
        self.refraction(snapshot, area, shape, shortest)
        snapshot.pop()
        # Tint keeps foreground legible over any backdrop.
        snapshot.append_color(self.tint, area)
        # Light gathers inside the edge: most under the lit top, some at the far bottom.
        snapshot.push_mask(Gsk.MaskMode.ALPHA)
        snapshot.push_blur(6)
        snapshot.append_stroke(shape, Gsk.Stroke.new(14), white)
        snapshot.pop()
        snapshot.pop()
        top = Graphene.Point().init(area.get_x(), area.get_y())
        bottom = Graphene.Point().init(area.get_x(), area.get_y() + area.get_height())
        snapshot.append_linear_gradient(area, top, bottom, gradient_stops((
            (0.0, rgba(1, 1, 1, 0.16 * emphasis)),
            (0.4, rgba(1, 1, 1, 0.02)),
            (0.75, rgba(1, 1, 1, 0.02)),
            (1.0, rgba(1, 1, 1, 0.1 * emphasis)),
        )))
        snapshot.pop()
        snapshot.pop()
        # Specular rim: a faint full outline, a soft glow and a crisp highlight.
        snapshot.push_mask(Gsk.MaskMode.ALPHA)
        snapshot.append_stroke(shape, Gsk.Stroke.new(1.0), white)
        snapshot.pop()
        snapshot.append_color(rgba(1, 1, 1, 0.1 * emphasis), area)
        snapshot.pop()
        self.specular(snapshot, area, shape, 6, 0.28 * self.rim * emphasis, blur=3)
        self.specular(snapshot, area, shape, 1.4, self.rim * emphasis)


# Touch response: the slab swells slightly and its light intensifies.
PRESS_SCALE = 0.03
PRESS_DURATION = 0.18


class GlassButton(Gtk.Button):
    """An ordinary accessible GTK button with a recovered glass background."""

    def __init__(self, source, radius=999, **properties):
        super().__init__(**properties)
        self.painter = GlassPainter(self, source, radius, rgba(.08, .08, .1, .3), blur=3)
        self.add_css_class('glass-control')
        if source is not None and hasattr(source, 'watch_glass'):
            source.watch_glass(self)

    def do_snapshot(self, snapshot):
        left, top, right, bottom = css_padding(self)
        area = rect(-left, -top, self.get_width() + left + right, self.get_height() + top + bottom)
        flags = self.get_state_flags()
        active = bool(flags & Gtk.StateFlags.ACTIVE)
        hover = bool(flags & Gtk.StateFlags.PRELIGHT)
        disabled = not self.is_sensitive()
        if disabled:
            snapshot.push_opacity(.4)
        self.painter.draw(snapshot, area, emphasis=1.35 if active else (1.12 if hover else 1))
        if self.has_css_class('suggested-action'):
            snapshot.push_fill(shape_path(area, self.painter.radius), Gsk.FillRule.WINDING)
            snapshot.append_color(rgba(.04, .52, 1, .36 if active else .28), area)
            snapshot.pop()
        if disabled:
            snapshot.pop()
        # GTK still draws the label, icon, focus ring and disabled state, and
        # retains its normal pointer/keyboard activation and accessible role.
        Gtk.Button.do_snapshot(self, snapshot)


class GlassBox(Gtk.Box):
    """A box whose own background is a liquid-glass slab."""

    def __init__(self, source, radius=28, tint=None, **properties):
        super().__init__(**properties)
        tint = tint or rgba(0.08, 0.08, 0.1, 0.3)
        self.painter = GlassPainter(self, source, radius, tint)
        if source is not None and hasattr(source, 'watch_glass'):
            source.watch_glass(self)
        self.add_css_class('glass')
        self.pressed = False
        self.press_level = 0.0
        self.press_from = 0.0
        self.press_started = 0.0
        self.press_tick = 0
        # Observe touches without claiming them, so children still get taps.
        touch = Gtk.GestureClick()
        touch.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        self.touch_point = None
        touch.connect('pressed', self.on_touch)
        touch.connect('released', lambda *_: self.set_pressed(False))
        touch.connect('cancel', lambda *_: self.set_pressed(False))
        touch.connect('unpaired-release', lambda *_: self.set_pressed(False))
        self.add_controller(touch)

    def on_touch(self, _gesture, _count, x, y):
        self.touch_point = (x, y)
        self.set_pressed(True)

    def set_pressed(self, pressed):
        if pressed == self.pressed:
            return
        self.pressed = pressed
        self.press_from = self.press_level
        self.press_started = GLib.get_monotonic_time() / 1e6
        if self.press_tick == 0:
            self.press_tick = self.add_tick_callback(self.on_press_tick)

    def on_press_tick(self, _widget, _clock):
        t = (GLib.get_monotonic_time() / 1e6 - self.press_started) / PRESS_DURATION
        target = 1.0 if self.pressed else 0.0
        self.press_level = self.press_from + (target - self.press_from) * ease(t)
        self.queue_draw()
        if t >= 1.0:
            self.press_level = target
            self.press_tick = 0
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    def push_press(self, snapshot):
        """Scale about the centre while pressed; returns whether to restore."""
        if self.press_level <= 0.001:
            return False
        scale = 1.0 + PRESS_SCALE * self.press_level
        centre = Graphene.Point().init(self.get_width() / 2, self.get_height() / 2)
        snapshot.save()
        snapshot.translate(centre)
        snapshot.scale(scale, scale)
        snapshot.translate(Graphene.Point().init(-centre.x, -centre.y))
        return True

    def surface(self):
        left, top, right, bottom = css_padding(self)
        return rect(-left, -top, self.get_width() + left + right, self.get_height() + top + bottom)

    def do_snapshot(self, snapshot):
        scaled = self.push_press(snapshot)
        surface = self.surface()
        self.painter.draw(snapshot, surface, emphasis=1.0 + 0.35 * self.press_level)
        self.painter.glow(snapshot, surface, self.touch_point, self.press_level)
        child = self.get_first_child()
        while child is not None:
            self.snapshot_child(child, snapshot)
            child = child.get_next_sibling()
        if scaled:
            snapshot.restore()


class GlassTabBar(GlassBox):
    """Floating tab bar with a sliding, stretching selection droplet."""

    def __init__(self, source):
        super().__init__(source, radius=999, tint=rgba(0.08, 0.08, 0.1, 0.38), spacing=4)
        # The selection is a clearer, neutral lens that magnifies what it covers.
        self.droplet = GlassPainter(self, source, 999, rgba(1, 1, 1, 0.12), blur=3, magnify=1.12, rim=1.0, saturate=1.1)
        self.selected = None
        self.start_rect = None
        self.target_rect = None
        self.started_at = 0
        self.tick_id = 0
        self.add_css_class('tab-bar')

    def button_rect(self, button):
        ok, bounds = button.compute_bounds(self)
        if not ok:
            return None
        return bounds.get_x(), bounds.get_y(), bounds.get_width(), bounds.get_height()

    def select(self, button):
        current = self.current_rect()
        self.selected = button
        self.start_rect = current
        self.target_rect = None
        self.started_at = GLib.get_monotonic_time() / 1e6
        # Animate only a visible move; the first selection simply appears.
        if current is not None and self.get_mapped() and self.tick_id == 0:
            self.tick_id = self.add_tick_callback(self.on_tick)
        self.queue_draw()

    def on_tick(self, _widget, _clock):
        self.queue_draw()
        if GLib.get_monotonic_time() / 1e6 - self.started_at >= DROPLET_DURATION:
            self.tick_id = 0
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    def progress(self):
        if self.start_rect is None:
            return 1.0
        return min(1.0, (GLib.get_monotonic_time() / 1e6 - self.started_at) / DROPLET_DURATION)

    def current_rect(self):
        if self.selected is None:
            return None
        target = self.button_rect(self.selected)
        if target is None:
            return None
        start = self.start_rect
        t = self.progress()
        if start is None or t >= 1.0:
            return target
        k = ease(t)
        x, y, width, height = (a + (b - a) * k for a, b in zip(start, target))
        # The droplet stretches along its motion and recovers on arrival.
        stretch = math.sin(math.pi * t) * 0.22
        horizontal = abs(target[0] - start[0]) >= abs(target[1] - start[1])
        if horizontal:
            grow = width * stretch
            return x - grow / 2, y + height * stretch * 0.15, width + grow, height * (1 - stretch * 0.3)
        grow = height * stretch
        return x + width * stretch * 0.15, y - grow / 2, width * (1 - stretch * 0.3), height + grow

    def do_snapshot(self, snapshot):
        scaled = self.push_press(snapshot)
        # Concentric corners keep an even gutter at either end of the dock.
        # A full capsule here would be rounder than the shorter tab inside it.
        target = None if self.selected is None else self.button_rect(self.selected)
        if target is not None and min(target[2:]) > 0:
            self.painter.radius = min(target[2:]) / 2 + min(css_padding(self))
        self.painter.draw(snapshot, self.surface(), emphasis=1.0 + 0.35 * self.press_level)
        droplet = self.current_rect()
        if droplet is not None:
            x, y, width, height = droplet
            self.droplet.draw(snapshot, rect(x, y, width, height), emphasis=0.7 + 0.3 * self.press_level)
        child = self.get_first_child()
        while child is not None:
            self.snapshot_child(child, snapshot)
            child = child.get_next_sibling()
        if scaled:
            snapshot.restore()


class OrientationBox(Gtk.Box):
    """A box that re-arranges itself for a landscape or portrait window.

    The window is sized to its output, so its requested size decides the
    arrangement. Watching that size, rather than every layout pass, keeps
    layout native; a landscape minimum could also otherwise keep a portrait
    window from ever becoming narrow enough to flip.
    """

    def __init__(self, **properties):
        super().__init__(**properties)
        self.landscape = None
        self.window = None
        self.handlers = []

    def arrange(self, landscape):
        raise NotImplementedError

    def apply(self, landscape):
        if landscape != self.landscape:
            self.landscape = landscape
            self.arrange(landscape)

    def follow_window(self, *_):
        width, height = self.window.get_default_size()
        if width > 0 and height > 0:
            self.apply(width >= height)

    def do_realize(self):
        Gtk.Box.do_realize(self)
        root = self.get_root()
        if isinstance(root, Gtk.Window) and root is not self.window:
            self.forget_window()
            self.window = root
            for name in ('notify::default-width', 'notify::default-height'):
                self.handlers.append(root.connect(name, self.follow_window))
        if self.window is not None:
            self.follow_window()

    def forget_window(self):
        for handler in self.handlers:
            self.window.disconnect(handler)
        self.handlers = []
        self.window = None


class AdaptiveShell(OrientationBox):
    """CarPlay placement: tab bar left in landscape, at the bottom in portrait."""

    def __init__(self, bar, content):
        super().__init__(orientation=Gtk.Orientation.HORIZONTAL)
        self.bar = bar
        self.content = content
        self.append(bar)
        self.append(content)
        self.apply(True)

    def arrange(self, landscape):
        if landscape:
            self.set_orientation(Gtk.Orientation.HORIZONTAL)
            self.reorder_child_after(self.content, self.bar)
            self.bar.set_orientation(Gtk.Orientation.VERTICAL)
            self.bar.set_valign(Gtk.Align.CENTER)
            self.bar.set_halign(Gtk.Align.START)
            self.bar.add_css_class('vertical')
            self.bar.remove_css_class('horizontal')
        else:
            self.set_orientation(Gtk.Orientation.VERTICAL)
            self.reorder_child_after(self.bar, self.content)
            self.bar.set_orientation(Gtk.Orientation.HORIZONTAL)
            self.bar.set_valign(Gtk.Align.END)
            self.bar.set_halign(Gtk.Align.CENTER)
            self.bar.add_css_class('horizontal')
            self.bar.remove_css_class('vertical')
        self.content.set_margin_start(28 if landscape else 40)
        self.content.set_margin_end(56 if landscape else 40)
        self.add_css_class('landscape' if landscape else 'portrait')
        self.remove_css_class('portrait' if landscape else 'landscape')
