# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native Home dashboard and settings sections, on glass over an artwork-tinted backdrop."""
import math
import time
import weakref
import gi

gi.require_version('GdkPixbuf', '2.0')
gi.require_version('Gtk', '4.0')
gi.require_version('Gsk', '4.0')
from gi.repository import Gdk, GdkPixbuf, GLib, Graphene, Gsk, Gtk, Pango
from phone_glass import GlassBox, GlassButton, gradient_stops, OrientationBox, rect, rgba, shape_path
from phone_icons import app_icon, icon

# Wall-clock years before this predate the software; such a clock is unset.
EARLIEST_CLOCK_YEAR = 2025
# Artwork is reduced to this many pixels per side. GTK's linear scaling of
# the tiny texture produces a soft colour field without a per-frame blur.
AMBIENT_SIZE = 6
# Night-sky fallback, ordered as a 3 x 3 grid from the top-left corner.
DEFAULT_AMBIENT = ((18, 32, 74), (38, 26, 88), (12, 22, 52),
                   (10, 58, 96), (34, 40, 110), (60, 22, 82),
                   (6, 20, 40), (16, 46, 78), (28, 18, 56))
APPS = (('maps', 'Maps'), ('music', 'Music'), ('phone', 'Phone'))


def text(value, style=None, xalign=0):
    item = Gtk.Label(label=value, xalign=xalign)
    item.set_ellipsize(Pango.EllipsizeMode.END)
    if style:
        item.add_css_class(style)
    return item


def clock_strings(now=None):
    """Return (time, date) for a set wall clock, or None when it is unset."""
    moment = time.localtime(now)
    if moment.tm_year < EARLIEST_CLOCK_YEAR:
        return None
    hour_minute = time.strftime('%H:%M', moment)
    weekday = time.strftime('%A', moment)
    month = time.strftime('%B', moment)
    return hour_minute, f'{weekday} {moment.tm_mday} {month}'


def ambient_pixels(rgba, width, height, channels):
    """Average an image into AMBIENT_SIZE² cells, then lift saturation."""
    size = AMBIENT_SIZE
    result = bytearray()
    for row in range(size):
        for column in range(size):
            top = row * height // size
            bottom = max(top + 1, (row + 1) * height // size)
            left = column * width // size
            right = max(left + 1, (column + 1) * width // size)
            totals = [0, 0, 0]
            count = 0
            # Sample a bounded lattice inside each cell; artwork is ≤1024 px.
            step_y = max(1, (bottom - top) // 8)
            step_x = max(1, (right - left) // 8)
            for y in range(top, bottom, step_y):
                for x in range(left, right, step_x):
                    offset = y * rgba[1] + x * channels
                    for index in range(3):
                        totals[index] += rgba[0][offset + index]
                    count += 1
            red, green, blue = (value / count for value in totals)
            mean = (red + green + blue) / 3
            # Vivid but darkened, so white text keeps its contrast. Lower
            # rows darken further behind the dock; a smooth texture avoids
            # the banding of a translucent full-window gradient.
            shade = 0.7 - 0.28 * row / (size - 1)
            for value in (red, green, blue):
                lifted = mean + (value - mean) * 1.5
                result.append(max(0, min(255, int(lifted * shade))))
    return bytes(result)


def ambient_texture(pixels, size):
    data = GLib.Bytes.new(pixels)
    return Gdk.MemoryTexture.new(size, size, Gdk.MemoryFormat.R8G8B8, data, size * 3)


def default_ambient():
    pixels = bytes(channel for colour in DEFAULT_AMBIENT for channel in colour)
    return ambient_texture(pixels, 3)


def artwork_pixels(raw):
    """AMBIENT_SIZE² RGB cells from verified PNG bytes; None if they do not decode."""
    loader = GdkPixbuf.PixbufLoader.new_with_type('png')
    try:
        loader.write(raw)
        loader.close()
    except GLib.Error:
        return None
    pixbuf = loader.get_pixbuf()
    if pixbuf is None or pixbuf.get_bits_per_sample() != 8 or pixbuf.get_n_channels() < 3:
        return None
    rgba = (pixbuf.get_pixels(), pixbuf.get_rowstride())
    return ambient_pixels(rgba, pixbuf.get_width(), pixbuf.get_height(), pixbuf.get_n_channels())


def artwork_ambient(raw):
    """Build the backdrop colour field from verified PNG bytes; None if they do not decode."""
    pixels = artwork_pixels(raw)
    return None if pixels is None else ambient_texture(pixels, AMBIENT_SIZE)


# Wallpaper shapes: centre (fractions of width and height) and radius
# (fraction of height). Their edges give the glass something to bend.
ORBS = ((0.06, 1.02, 0.66), (0.92, 0.02, 0.58), (0.62, 0.98, 0.4))


def orb_palette(pixels, count=len(ORBS)):
    """The most vivid, mutually distinct colours of a colour field, brightened."""
    cells = [tuple(pixels[i:i + 3]) for i in range(0, len(pixels) - 2, 3)]
    def vividness(colour):
        high = max(colour)
        low = min(colour)
        return (high - low) * high
    chosen = []
    for colour in sorted(cells, key=vividness, reverse=True):
        if all(sum(abs(a - b) for a, b in zip(colour, other)) > 90 for other in chosen):
            chosen.append(colour)
        if len(chosen) == count:
            break
    while len(chosen) < count:
        chosen.append(chosen[-1] if chosen else (40, 60, 140))
    result = []
    for colour in chosen:
        lift = 200 / max(1, max(colour))
        result.append(tuple(min(255, int(channel * lift)) / 255 for channel in colour))
    return result


# Darkening over the colour field, so white text keeps its contrast.
SCRIM = (0.0, 0.0, 0.0, 0.35)


class BackdropWidget(Gtk.Widget):
    def __init__(self, backdrop):
        super().__init__()
        self.backdrop = backdrop
        self.set_hexpand(True)
        self.set_vexpand(True)
        self.set_can_target(False)

    def do_snapshot(self, snapshot):
        self.backdrop.paint(snapshot, self.get_width(), self.get_height())


class Backdrop:
    """Full-window colour field behind the glass interface.

    Glass surfaces call paint() directly to see through to it. Drawing the
    texture is cheap and, unlike re-rendering a widget, never schedules
    another frame.
    """

    def __init__(self):
        self.default = default_ambient()
        self.default_palette = orb_palette(bytes(c for colour in DEFAULT_AMBIENT for c in colour))
        self.texture = self.default
        self.palette = self.default_palette
        self.source = None
        self.revision = 0
        self.glass_surfaces = weakref.WeakSet()
        self.widget = BackdropWidget(self)
        self.widget.connect('notify::visible', self.invalidate_glass)

    def watch_glass(self, surface):
        self.glass_surfaces.add(surface)

    def invalidate_glass(self, *_):
        for surface in self.glass_surfaces:
            surface.queue_draw()

    def paint(self, snapshot, width, height):
        """Cover-fit the colour field into width x height, then darken it."""
        texture_width = self.texture.get_width()
        texture_height = self.texture.get_height()
        scale = max(width / texture_width, height / texture_height)
        drawn_width = texture_width * scale
        drawn_height = texture_height * scale
        area = Graphene.Rect().init((width - drawn_width) / 2, (height - drawn_height) / 2, drawn_width, drawn_height)
        snapshot.append_scaled_texture(self.texture, Gsk.ScalingFilter.LINEAR, area)
        for (x, y, size), (red, green, blue) in zip(ORBS, self.palette):
            radius = size * height
            centre = Graphene.Point().init(x * width, y * height)
            bounds = Graphene.Rect().init(centre.x - radius, centre.y - radius, radius * 2, radius * 2)
            stops = gradient_stops([(offset, rgba(red, green, blue, alpha))
                                    for offset, alpha in ((0.0, 0.95), (0.7, 0.8), (0.93, 0.6), (1.0, 0.0))])
            snapshot.append_radial_gradient(bounds, centre, radius, radius, 0, 1, stops)
        scrim = Gdk.RGBA()
        scrim.red, scrim.green, scrim.blue, scrim.alpha = SCRIM
        snapshot.append_color(scrim, Graphene.Rect().init(0, 0, width, height))

    def set_texture(self, texture, palette=None):
        self.texture = texture
        self.palette = palette or self.default_palette
        self.revision += 1
        self.widget.queue_draw()
        self.invalidate_glass()

    def show_artwork(self, digest, raw):
        if digest == self.source:
            return
        pixels = artwork_pixels(raw)
        self.source = digest if pixels is not None else None
        if pixels is None:
            self.set_texture(self.default)
        else:
            self.set_texture(ambient_texture(pixels, AMBIENT_SIZE), orb_palette(pixels))

    def clear(self):
        if self.source is None:
            return
        self.source = None
        self.set_texture(self.default)


class HomeLayout(OrientationBox):
    def __init__(self, view):
        super().__init__(spacing=24)
        self.view = view

    def arrange(self, landscape):
        view = self.view
        self.set_orientation(Gtk.Orientation.HORIZONTAL if landscape else Gtk.Orientation.VERTICAL)
        self.set_spacing(24)
        view.left.set_hexpand(not landscape)
        view.left.set_size_request(336 if landscape else -1, 336)
        view.right.set_size_request(640 if landscape else -1, 336)


class HomeView:
    """The landing page. It reuses the page's guarded actions for every tap."""

    def __init__(self, owner):
        self.owner = owner
        self.clock_text = None
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=28)
        self.widget.add_css_class('home-view')
        self.widget.set_valign(Gtk.Align.CENTER)

        heading = Gtk.Box(spacing=24)
        self.widget.append(heading)
        clock = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        clock.set_hexpand(True)
        heading.append(clock)
        self.clock = text('', 'home-clock')
        clock.append(self.clock)
        self.date = text('', 'home-date')
        clock.append(self.date)
        connection = self.build_phone()
        connection.set_valign(Gtk.Align.CENTER)
        heading.append(connection)

        self.layout = HomeLayout(self)
        self.widget.append(self.layout)
        left = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.left = left
        left.set_hexpand(True)
        self.layout.append(left)
        left.append(self.build_apps())

        right = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.right = right
        right.set_hexpand(True)
        self.layout.append(right)
        self.player = self.build_now_playing()
        self.player.set_vexpand(True)
        right.append(self.player)
        self.layout.apply(True)
        self.tick()

    def build_now_playing(self):
        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        content.set_vexpand(True)
        content.add_css_class('now-playing')
        # The track opens Music; transport buttons are siblings so their taps
        # cannot also activate an enclosing button.
        track = Gtk.Button()
        self.track = track
        track.add_css_class('widget-track')
        track.update_property([Gtk.AccessibleProperty.LABEL], ['Open Now Playing'])
        track.connect('clicked', lambda _: self.owner.view_buttons['music'].set_active(True))
        content.append(track)
        top = Gtk.Box(spacing=18)
        top.set_valign(Gtk.Align.CENTER)
        track.set_child(top)
        # An image scales the artwork to a fixed square, whatever its size.
        self.cover = Gtk.Image()
        self.cover.set_pixel_size(112)
        self.cover_stack = Gtk.Stack()
        self.cover_stack.add_css_class('widget-artwork')
        self.cover_stack.set_overflow(Gtk.Overflow.HIDDEN)
        self.cover_stack.set_valign(Gtk.Align.CENTER)
        self.cover_stack.add_named(self.cover, 'art')
        empty = icon('music', 44, color='#8e8e93')
        empty.set_size_request(112, 112)
        self.cover_stack.add_named(empty, 'empty')
        top.append(self.cover_stack)
        details = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        details.set_valign(Gtk.Align.CENTER)
        details.set_hexpand(True)
        top.append(details)
        self.title = text('Nothing playing', 'widget-title')
        self.title.add_css_class('widget-track-title')
        details.append(self.title)
        self.subtitle = text('', 'widget-subtitle')
        self.subtitle.add_css_class('widget-track-artist')
        details.append(self.subtitle)
        space = Gtk.Box()
        space.set_vexpand(True)
        self.media_spacers = [space]
        content.append(space)
        self.timeline = Gtk.Box(spacing=12)
        content.append(self.timeline)
        self.elapsed = text('—:—', 'media-time')
        self.timeline.append(self.elapsed)
        self.progress = Gtk.ProgressBar()
        self.progress.add_css_class('glass-progress')
        self.progress.set_hexpand(True)
        self.progress_surface = GlassBox(self.source_layer(), radius=999)
        self.progress_surface.set_hexpand(True)
        self.progress_surface.set_valign(Gtk.Align.CENTER)
        self.progress_surface.append(self.progress)
        self.timeline.append(self.progress_surface)
        self.remaining = text('—:—', 'media-time')
        self.timeline.append(self.remaining)
        space = Gtk.Box()
        space.set_vexpand(True)
        self.media_spacers.append(space)
        content.append(space)
        self.transport = Gtk.Box(spacing=16, homogeneous=True)
        content.append(self.transport)
        self.buttons = {}
        for command in ('previous', 'play', 'pause', 'next'):
            button = Gtk.Button()
            button.add_css_class('widget-transport')
            button.set_hexpand(True)
            size = 68 if command in ('play', 'pause') else 56
            button.set_child(icon(command, size, color='#ffffff'))
            button.update_property([Gtk.AccessibleProperty.LABEL], [command.capitalize()])
            button.connect('clicked', lambda _, value=command: self.owner.media_action(value))
            self.transport.append(button)
            self.buttons[command] = button
        slab = GlassBox(self.source_layer(), radius=30)
        slab.append(content)
        content.set_hexpand(True)
        return slab

    def build_apps(self):
        self.apps = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, homogeneous=True)
        self.apps.set_vexpand(True)
        self.app_buttons = {}
        descriptions = {'maps': 'Find a destination', 'music': 'Your music library',
                        'phone': 'Contacts and calls'}
        for app, title in APPS:
            button = GlassButton(self.source_layer(), radius=24)
            button.add_css_class('home-shortcut')
            button.update_property([Gtk.AccessibleProperty.LABEL], ['Open ' + title])
            row = Gtk.Box(spacing=16)
            row.set_valign(Gtk.Align.CENTER)
            button.set_child(row)
            row.append(app_icon(app, 56))
            names = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
            names.set_hexpand(True)
            names.set_valign(Gtk.Align.CENTER)
            row.append(names)
            names.append(text(title, 'home-shortcut-title'))
            names.append(text(descriptions[app], 'home-shortcut-detail'))
            chevron = text('›', 'home-shortcut-chevron')
            row.append(chevron)
            button.connect('clicked', lambda _, value=app: self.owner.open_phone_app(value))
            self.apps.append(button)
            self.app_buttons[app] = button
        return self.apps

    def build_phone(self):
        card = Gtk.Box(spacing=14)
        card.add_css_class('home-connection')
        self.phone_badge = icon('phone', 20, color='#ffffff')
        self.phone_badge.add_css_class('widget-badge')
        self.phone_badge.set_valign(Gtk.Align.CENTER)
        card.append(self.phone_badge)
        names = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        names.set_valign(Gtk.Align.CENTER)
        card.append(names)
        self.phone_name = text('No phone', 'widget-title')
        self.phone_name.set_max_width_chars(18)
        names.append(self.phone_name)
        state = Gtk.Box(spacing=7)
        names.append(state)
        self.phone_dot = Gtk.Box()
        self.phone_dot.add_css_class('status-dot')
        self.phone_dot.set_valign(Gtk.Align.CENTER)
        state.append(self.phone_dot)
        self.phone_state = text('', 'widget-subtitle')
        state.append(self.phone_state)
        self.phone_button = GlassButton(self.source_layer(), label='Phones')
        self.phone_button.add_css_class('widget-action')
        self.phone_button.set_valign(Gtk.Align.CENTER)
        self.phone_button.connect('clicked', self.phone_clicked)
        card.append(self.phone_button)
        return card

    def source_layer(self):
        return getattr(self.owner, 'backdrop', None)

    def phone_clicked(self, _):
        if self.owner.can_return():
            self.owner.back()
            return
        self.owner.view_buttons['phones'].set_active(True)

    def tick(self, now=None):
        strings = clock_strings(now)
        if strings == self.clock_text:
            return
        self.clock_text = strings
        self.clock.set_visible(strings is not None)
        self.date.set_visible(strings is not None)
        if strings is not None:
            self.clock.set_text(strings[0])
            self.date.set_text(strings[1])

    def render(self, data, session, texture):
        media = data.get('media') or {}
        available = bool(media.get('available'))
        if available:
            self.title.set_text(media.get('title') or 'Unknown title')
            self.subtitle.set_text(media.get('artist') or media.get('album') or '')
        else:
            self.title.set_text('Nothing playing')
            self.subtitle.set_text('Play something on your iPhone.' if data.get('active') else 'Connect your iPhone.')
        self.cover.set_from_paintable(texture)
        self.cover_stack.set_visible_child_name('art' if texture is not None else 'empty')
        position = media.get('position_ms')
        duration = media.get('duration_ms')
        timed = available and type(position) is int and type(duration) is int and 0 <= position and 0 < duration
        self.progress.set_visible(timed)
        self.progress_surface.set_visible(timed)
        self.progress.set_fraction(min(1, position / duration) if timed else 0)
        # Only observed timing is shown. Never run a local playback clock or
        # invent a remaining duration when the phone has not supplied both.
        def stamp(value):
            minutes, seconds = divmod(value // 1000, 60)
            return f'{minutes}:{seconds:02d}'

        self.timeline.set_visible(timed)
        self.elapsed.set_hexpand(not timed)
        known_position = available and type(position) is int and position >= 0
        self.elapsed.set_text(stamp(position) if known_position else '—:—')
        self.remaining.set_text('−' + stamp(max(0, duration - position)) if timed else '—:—')
        controls = media.get('controls', []) if available else []
        status = media.get('status')
        for command, button in self.buttons.items():
            supported = command in controls
            redundant = (command == 'play' and status == 'playing' and 'pause' in controls) or (
                command == 'pause' and status in ('paused', 'stopped') and 'play' in controls)
            button.set_visible(supported and not redundant)
        transport_visible = any(button.get_visible() for button in self.buttons.values())
        self.transport.set_visible(transport_visible)
        self.track.set_vexpand(not (timed or transport_visible))
        for space in self.media_spacers:
            space.set_visible(timed or transport_visible)
        self.render_phone(data, session)

    def render_phone(self, data, session):
        active = data.get('active')
        phone = next((item for item in data.get('phones', []) if item.get('address') == active), None)
        live = False
        if phone is None:
            self.phone_name.set_text('No phone')
            state = 'Not connected'
        else:
            self.phone_name.set_text(phone['name'])
            mode = data.get('active_mode')
            if session in ('disconnected', 'stopped'):
                state = 'Waiting for iPhone'
            elif session == 'starting':
                state = 'Connecting…'
            elif session == 'fault':
                state = 'CarPlay stopped'
            elif mode == 'ultra':
                state, live = 'CarPlay Ultra', True
            elif mode == 'standard':
                state, live = 'CarPlay', True
            else:
                state = 'Connecting…'
        self.phone_state.set_text(state)
        if live:
            self.phone_dot.add_css_class('live')
            self.phone_badge.remove_css_class('offline')
        else:
            self.phone_dot.remove_css_class('live')
            self.phone_badge.add_css_class('offline')
        returnable = self.owner.can_return()
        self.phone_button.set_label('Open CarPlay' if returnable else 'Phones')
        if returnable:
            self.phone_button.add_css_class('suggested-action')
        else:
            self.phone_button.remove_css_class('suggested-action')

    def set_shortcuts(self, available):
        for button in self.app_buttons.values():
            button.set_sensitive(available)

    def lost(self):
        self.render({'media': {}}, None, None)
        self.phone_name.set_text('Phone status unavailable')
        self.phone_state.set_text('Waiting for the connection manager…')
        self.set_shortcuts(False)


# Settings sections: cluster formats, drive-mode display profiles and the greeting.
# Saved preferences show as chosen; nothing here claims the cluster, the car
# or a display applied them unless the manager reports that.

# Native gauge formats replace the whole cluster; content pages fill its centre.
FORMATS = (('minimal', 'Minimal'), ('sport', 'Sport'), ('gauges', 'Full Gauges'))
CONTENT = (('map', 'Map', 'map', 'tint-blue'), ('media', 'Media', 'music', 'tint-pink'),
           ('vehicle', 'Vehicle', 'vehicle', 'tint-indigo'), ('trip', 'Trip', 'trip', 'tint-orange'))
GREETINGS = (('off', 'Off'), ('brand', 'Project emblem'), ('minimal', 'Minimal'))
# Drive-mode publication phases, in words. Requested is never applied.
DRIVE_PHASES = {
    'waiting': 'Waiting for the cluster',
    'pending': 'Applying to the cluster…',
    'confirmed': 'Shown on the cluster',
    'partial': 'Partly shown: the cluster kept some of its own look',
    'unsupported': 'The current cluster style cannot show this look',
    'changed-on-phone': 'Changed on the iPhone',
    'unavailable': 'Cluster unavailable',
    'native-profile': 'Chosen for the native cluster',
    'local-profile': 'Chosen for the cluster',
}
PREVIEW_WIDTH = 248
PREVIEW_HEIGHT = 112
ACCENT = rgba(10 / 255, 132 / 255, 1, 1)
WHITE = rgba(1, 1, 1, 1)
DIM = rgba(1, 1, 1, 0.16)
SECONDARY = rgba(235 / 255, 235 / 255, 245 / 255, 0.6)
REDLINE = rgba(1, 69 / 255, 58 / 255, 1)


def arc_path(cx, cy, radius, start, end):
    """Clockwise arc between two angles in degrees, 0 pointing right."""
    x0 = cx + radius * math.cos(math.radians(start))
    y0 = cy + radius * math.sin(math.radians(start))
    x1 = cx + radius * math.cos(math.radians(end))
    y1 = cy + radius * math.sin(math.radians(end))
    large = 1 if end - start > 180 else 0
    return Gsk.Path.parse(f'M {x0} {y0} A {radius} {radius} 0 {large} 1 {x1} {y1}')


def round_stroke(width):
    stroke = Gsk.Stroke.new(width)
    stroke.set_line_cap(Gsk.LineCap.ROUND)
    return stroke


def segmented(choices, clicked):
    """An iOS segmented control; returns (box, {value: button})."""
    box = Gtk.Box(spacing=0, homogeneous=True)
    box.add_css_class('segmented')
    box.set_halign(Gtk.Align.START)
    buttons = {}
    for value, title in choices:
        button = Gtk.Button(label=title)
        button.add_css_class('segment')
        button.set_size_request(150, -1)
        button.connect('clicked', lambda _, choice=value: clicked(choice))
        box.append(button)
        buttons[value] = button
    return box, buttons


def mark(buttons, selected):
    for value, button in buttons.items():
        if value == selected:
            button.add_css_class('selected')
        else:
            button.remove_css_class('selected')


class FormatPreview(Gtk.Widget):
    """A miniature of one native cluster format, ringed when chosen."""

    def __init__(self, format_id):
        super().__init__()
        self.format_id = format_id
        self.selected = False
        self.set_size_request(PREVIEW_WIDTH, PREVIEW_HEIGHT)

    def set_selected(self, selected):
        if selected != self.selected:
            self.selected = selected
            self.queue_draw()

    def number(self, snapshot, value, size, cx, cy, colour, weight=600):
        layout = self.create_pango_layout(value)
        font = Pango.FontDescription.from_string(f'sans-serif {size}px')
        font.set_weight(weight)
        layout.set_font_description(font)
        _, extent = layout.get_pixel_extents()
        snapshot.save()
        snapshot.translate(Graphene.Point().init(cx - extent.width / 2 - extent.x, cy - extent.height / 2 - extent.y))
        snapshot.append_layout(layout, colour)
        snapshot.restore()

    def do_snapshot(self, snapshot):
        width = self.get_width()
        height = self.get_height()
        if self.selected:
            snapshot.append_stroke(shape_path(rect(1.5, 1.5, width - 3, height - 3), 22), Gsk.Stroke.new(3), ACCENT)
        panel = rect(6, 6, width - 12, height - 12)
        outline = shape_path(panel, 17)
        snapshot.push_fill(outline, Gsk.FillRule.WINDING)
        snapshot.append_color(rgba(0.04, 0.04, 0.05, 1), panel)
        snapshot.pop()
        snapshot.append_stroke(outline, Gsk.Stroke.new(1), rgba(1, 1, 1, 0.1))
        cx = width / 2
        cy = height / 2
        if self.format_id == 'minimal':
            self.number(snapshot, '88', 46, cx, cy - 6, WHITE)
            self.number(snapshot, 'km/h', 11, cx, cy + 26, SECONDARY, 500)
            self.number(snapshot, 'D', 15, panel.get_x() + panel.get_width() - 26, cy, rgba(48 / 255, 209 / 255, 88 / 255, 1))
        elif self.format_id == 'sport':
            radius = panel.get_height() * 0.4
            dial_y = cy + 6
            snapshot.append_stroke(arc_path(cx, dial_y, radius, 150, 390), round_stroke(5), DIM)
            snapshot.append_stroke(arc_path(cx, dial_y, radius, 150, 300), round_stroke(5), WHITE)
            snapshot.append_stroke(arc_path(cx, dial_y, radius, 355, 390), round_stroke(5), REDLINE)
            self.number(snapshot, '88', 24, cx, dial_y - 2, WHITE)
        else:
            radius = panel.get_height() * 0.34
            snapshot.append_stroke(arc_path(cx, cy, radius, 135, 405), round_stroke(4), DIM)
            snapshot.append_stroke(arc_path(cx, cy, radius, 135, 260), round_stroke(4), WHITE)
            self.number(snapshot, '88', 18, cx, cy, WHITE)
            # Three auxiliary dials on each side, as on the native Full Gauges.
            small = 9
            sweeps = (150, 90, 200, 120, 60, 180)
            for index, (side, row) in enumerate((s, r) for s in (-1, 1) for r in (-1, 0, 1)):
                x = cx + side * (radius + 30)
                y = cy + row * 25
                snapshot.append_stroke(arc_path(x, y, small, 135, 405), round_stroke(2), DIM)
                snapshot.append_stroke(arc_path(x, y, small, 135, 135 + sweeps[index]), round_stroke(2), WHITE)


class ClusterFormats:
    """Native cluster choices: gauge formats as previews, content as a list."""

    def __init__(self, owner, list_group, list_row):
        self.owner = owner
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.widget.append(text('FORMAT', 'eyebrow'))
        self.faces = Gtk.Box(spacing=16)
        self.widget.append(self.faces)
        self.buttons = {}
        self.marks = {}
        self.previews = {}
        for page, title in FORMATS:
            button = Gtk.Button()
            button.add_css_class('format-face')
            column = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
            preview = FormatPreview(page)
            column.append(preview)
            name = Gtk.Label(label=title)
            name.add_css_class('format-name')
            column.append(name)
            button.set_child(column)
            button.update_property([Gtk.AccessibleProperty.LABEL], [title])
            button.connect('clicked', lambda _, value=page: self.choose(value))
            self.faces.append(button)
            self.buttons[page] = button
            self.previews[page] = preview
        self.content_heading = text('CENTRE CONTENT', 'eyebrow')
        self.widget.append(self.content_heading)
        self.content = list_group()
        self.widget.append(self.content)
        for page, title, symbol, tint in CONTENT:
            row = Gtk.Box(spacing=14)
            tile = icon(symbol, 18, '#ffffff')
            tile.add_css_class('row-icon')
            tile.add_css_class(tint)
            tile.set_valign(Gtk.Align.CENTER)
            row.append(tile)
            name = text(title, 'choice-label')
            name.set_hexpand(True)
            row.append(name)
            check = Gtk.Label(label='✓')
            check.add_css_class('selection-mark')
            check.set_visible(False)
            row.append(check)
            button = list_row(Gtk.Button())
            button.set_child(row)
            button.update_property([Gtk.AccessibleProperty.LABEL], [title])
            button.connect('clicked', lambda _, value=page: self.choose(value))
            self.content.append(button)
            self.buttons[page] = button
            self.marks[page] = check
        note = text('A format replaces the whole cluster. Warning lights always stay in place.', 'muted')
        note.add_css_class('small')
        note.set_wrap(True)
        self.widget.append(note)

    def choose(self, page):
        self.owner.action({'action': 'cluster_page', 'page': page})

    def render(self, pages, selected):
        """Show the manager's available pages; mark only its saved preference."""
        for page, button in self.buttons.items():
            available = page in pages
            button.set_sensitive(available)
            # Hide the grouped row (its flowbox child) or the face itself.
            parent = button.get_parent()
            (parent if isinstance(parent, Gtk.FlowBoxChild) else button).set_visible(available)
            chosen = available and page == selected
            if page in self.previews:
                self.previews[page].set_selected(chosen)
            if page in self.marks:
                self.marks[page].set_visible(chosen)
            if chosen:
                button.add_css_class('selected')
            else:
                button.remove_css_class('selected')
            title = dict(FORMATS + tuple(c[:2] for c in CONTENT))[page]
            button.update_property([Gtk.AccessibleProperty.LABEL], [title + (' · Selected' if chosen else '')])
        self.faces.set_visible(any(page in pages for page, _ in FORMATS))
        self.content_heading.set_visible(any(page in pages for page, *_ in CONTENT))

    def unmark(self):
        """Forget the selection when the manager's status expires."""
        for page, button in self.buttons.items():
            button.remove_css_class('selected')
            if page in self.previews:
                self.previews[page].set_selected(False)
            if page in self.marks:
                self.marks[page].set_visible(False)


class DriveModeSettings:
    """Pick the display profile for a drive mode. The car's own mode is never changed."""

    def __init__(self, owner):
        self.owner = owner
        self.revision = None
        self.mode_ids = ()
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self.widget.set_margin_bottom(14)
        self.widget.append(text('Drive mode', 'section'))
        note = text('Changes how the screens look. It never changes how the car drives.', 'muted')
        note.set_wrap(True)
        self.widget.append(note)
        self.slot = Gtk.Box()
        self.widget.append(self.slot)
        self.buttons = {}
        self.choices = None
        self.state = text('', 'muted')
        self.state.add_css_class('small')
        self.state.set_wrap(True)
        self.widget.append(self.state)

    def choose(self, mode):
        if self.revision is None:
            return
        self.owner.action({'action': 'drive_mode', 'mode': mode, 'revision': self.revision})

    def render(self, value):
        """Return whether drive modes are configured."""
        if type(value) is not dict or not value.get('configured') or not value.get('modes'):
            self.widget.set_visible(False)
            self.revision = None
            return False
        self.widget.set_visible(True)
        self.revision = value.get('revision') if type(value.get('revision')) is int else None
        choices = ((None, 'Automatic'), *((m['id'], m['name']) for m in value['modes']))
        if choices != self.choices:
            self.choices = choices
            child = self.slot.get_first_child()
            if child:
                self.slot.remove(child)
            box, self.buttons = segmented(choices, self.choose)
            for button in self.buttons.values():
                button.set_size_request(118, -1)
            self.slot.append(box)
        selected = value.get('selected')
        mark(self.buttons, selected)
        self.state.set_text(self.describe(value, selected))
        return True

    def describe(self, value, selected):
        if value.get('error'):
            return value['error']
        names = {m['id']: m['name'] for m in value['modes']}
        current = value.get('current')
        if type(current) is not dict:
            return 'Follows the car when it reports a mode.' if selected is None else 'Saved. Waiting for the cluster.'
        parts = []
        mode = names.get(current.get('mode'))
        if current.get('source') == 'vehicle' and mode:
            parts.append(f'Car reports {mode}')
        elif current.get('source') == 'simulation' and mode:
            parts.append(f'Simulated car mode {mode}')
        elif selected is None:
            parts.append('No mode reported by the car')
        phase = DRIVE_PHASES.get(current.get('phase'))
        if current.get('phase') == 'error':
            phase = current.get('error') or 'The cluster could not show this look'
        if phase:
            parts.append(phase)
        return ' · '.join(parts)


class GreetingSettings:
    """Choose the welcome/goodbye animation and preview it on the cluster."""

    def __init__(self, owner):
        self.owner = owner
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self.widget.set_margin_bottom(14)
        self.widget.append(text('Welcome animation', 'section'))
        self.note = text('Plays on the instrument cluster when you start and leave the car.', 'muted')
        self.note.set_wrap(True)
        self.widget.append(self.note)
        row = Gtk.Box(spacing=16)
        self.choices, self.buttons = segmented(
            GREETINGS, lambda variant: self.owner.action({'action': 'greeting_sequence', 'variant': variant}))
        row.append(self.choices)
        self.previews = {}
        for phase, title in (('startup', 'Preview welcome'), ('goodbye', 'Preview goodbye')):
            button = Gtk.Button(label=title)
            button.add_css_class('quiet')
            button.connect('clicked', lambda _, value=phase: self.owner.action({'action': 'greeting_preview', 'phase': value}))
            row.append(button)
            self.previews[phase] = button
        self.widget.append(row)

    def render(self, value):
        """Return whether the greeting is configured."""
        if type(value) is not dict or not value.get('configured'):
            self.widget.set_visible(False)
            return False
        self.widget.set_visible(True)
        variant = value.get('variant') if value.get('variant') in dict(GREETINGS) else None
        mark(self.buttons, variant)
        for button in self.previews.values():
            button.set_visible(variant not in (None, 'off'))
        return True


# Pre-phone vehicle readings (status.prephone_vehicle), as published by the
# native cluster source. Missing, unavailable, out-of-date and disconnected
# readings stay distinct; real zero and negative values are shown as read.
VEHICLE_STATES = {'missing': 'Not reported', 'unavailable': 'Unavailable',
                  'stale': 'Out of date', 'disconnected': 'Car disconnected'}
VEHICLE_TILES = (('fuel_percent', 'Fuel'), ('range_km', 'Range'), ('ambient_c', 'Outside'))
VEHICLE_LIST = (('coolant_c', 'Coolant'), ('oil_c', 'Oil temperature'), ('oil_pressure_kpa', 'Oil pressure'),
                ('battery_voltage', 'Battery'), ('odometer_km', 'Odometer'))
CAR_UNAVAILABLE = {
    'disabled': 'Car readings are not set up on this head unit.',
    'publication_unavailable': 'The car data service could not start.',
    'owner_retired': 'The car data service stopped.',
    'publisher_expired': 'The car data service stopped responding.',
    'protocol_failure': 'The car sent data this head unit could not read.',
    'clock_failure': 'Car readings have no trustworthy time.',
}


def vehicle_value(identifier, value):
    """A reading in display units; None when it cannot be shown."""
    if type(value) not in (int, float) or not math.isfinite(value):
        return None
    if identifier == 'fuel_percent':
        return f'{round(value)}%'
    if identifier == 'range_km':
        return f'{round(value)} km'
    if identifier == 'battery_voltage':
        return f'{value:.1f} V'
    if identifier in ('ambient_c', 'coolant_c', 'oil_c'):
        return f'{round(value)}°C'
    if identifier == 'oil_pressure_kpa':
        return f'{value / 100:.1f} bar'
    if identifier == 'odometer_km':
        return f'{round(value):,} km'.replace(',', ' ')
    return None


class CarView:
    """Large tiles for fuel, range and outside temperature; a list for the rest."""

    def __init__(self, list_group, list_row):
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.unavailable = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.unavailable.add_css_class('card')
        self.unavailable.append(text('Car readings unavailable', 'section'))
        self.reason = text('', 'muted')
        self.reason.set_wrap(True)
        self.unavailable.append(self.reason)
        self.widget.append(self.unavailable)
        self.simulated = text('SIMULATED DATA', 'simulated-badge')
        self.simulated.set_halign(Gtk.Align.START)
        self.simulated.set_ellipsize(Pango.EllipsizeMode.NONE)
        self.widget.append(self.simulated)
        tiles = Gtk.Box(spacing=16, homogeneous=True)
        self.widget.append(tiles)
        self.values = {}
        self.states = {}
        self.fuel = Gtk.ProgressBar()
        for identifier, title in VEHICLE_TILES:
            tile = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            tile.add_css_class('card')
            tile.add_css_class('car-tile')
            tile.append(text(title, 'widget-eyebrow'))
            value = text('—', 'car-value')
            tile.append(value)
            state = text('', 'muted')
            state.add_css_class('small')
            tile.append(state)
            if identifier == 'fuel_percent':
                tile.append(self.fuel)
            tiles.append(tile)
            self.values[identifier] = value
            self.states[identifier] = state
        self.details = list_group()
        self.widget.append(self.details)
        for identifier, title in VEHICLE_LIST:
            row = Gtk.Box(spacing=12)
            name = text(title, 'choice-label')
            name.set_hexpand(True)
            row.append(name)
            value = text('—', 'car-row-value')
            row.append(value)
            button = list_row(Gtk.Button())
            button.set_child(row)
            button.set_can_target(False)
            button.set_focusable(False)
            self.details.append(button)
            self.values[identifier] = value
        self.lost()

    def render(self, model):
        """Return whether the page should be offered."""
        if type(model) is not dict or not model.get('configured'):
            return False
        rows = {row.get('id'): row for row in model.get('rows', ()) if type(row) is dict}
        usable = model.get('state') in ('admitted', 'disconnected') and rows
        self.unavailable.set_visible(not usable)
        self.reason.set_text(CAR_UNAVAILABLE.get(model.get('state'), 'Waiting for the car.'))
        simulated = False
        for identifier, label in self.values.items():
            row = rows.get(identifier) if usable else None
            shown = None
            state = 'missing'
            if row is not None:
                state = row.get('availability')
                if state == 'available':
                    shown = vehicle_value(identifier, row.get('value'))
                    simulated = simulated or row.get('origin') == 'simulation'
            label.set_text(shown or '—')
            if shown is None:
                label.add_css_class('unavailable')
            else:
                label.remove_css_class('unavailable')
            if identifier in self.states:
                self.states[identifier].set_text('' if shown else VEHICLE_STATES.get(state, 'Unavailable'))
            elif shown is None and row is not None:
                label.set_text(VEHICLE_STATES.get(state, 'Unavailable'))
            if identifier == 'fuel_percent':
                self.fuel.set_visible(shown is not None)
                if shown is not None:
                    self.fuel.set_fraction(max(0.0, min(1.0, row['value'] / 100)))
        self.simulated.set_visible(simulated)
        return True

    def lost(self):
        """Forget readings when the manager's status expires."""
        self.render({'configured': True, 'state': 'awaiting_source', 'rows': []})
        self.reason.set_text('Waiting for the connection manager…')
