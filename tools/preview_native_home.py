#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Render the real GTK Home widgets using read-only synthetic design data.

No manager socket, phone, radio, camera or physical compositor is contacted.
Run locally, or inside an explicitly isolated Wayland fixture on Linux.
"""
import argparse
import copy
import json
from pathlib import Path
import time

import phone_page
from phone_page import Gtk, GLib, Gio


PHONE = 'AA:BB:CC:DD:EE:01'
STATUS = {
    'ok': True, 'active': PHONE, 'active_mode': 'ultra', 'generation': 1,
    'phones': [
        {'address': PHONE, 'name': 'Alex\u2019s iPhone', 'active': True, 'selected': True, 'connected': True},
        {'address': 'AA:BB:CC:DD:EE:02', 'name': 'Jamie\u2019s iPhone', 'active': False, 'selected': False, 'connected': False},
    ],
    'carplay': {'state': 'ready', 'returnReady': True},
    'cluster_pages': ['map', 'media', 'vehicle', 'trip'], 'cluster_page': 'media',
    'ultra_cluster': {'nonce': 'a' * 32, 'connectionGeneration': '1', 'active': '',
                      'pending': '', 'ready': True, 'idle': True, 'transitioning': False,
                      'error': '', 'choices': []}, 'error': '',
    'media': {'available': True, 'title': 'Midnight City', 'artist': 'M83',
              'album': 'Hurry Up, We\u2019re Dreaming', 'status': 'playing',
              'duration_ms': 243000, 'position_ms': 97000, 'controls': ['previous', 'pause', 'next']},
    'camera': {'configured': True, 'requested': False, 'phase': 'idle', 'error': ''},
    'pairing': {'supported': True, 'phase': 'idle', 'session': '', 'expiresInSeconds': 0,
                'adapterName': 'HEADUNIT', 'prompt': None, 'pairedAddress': '', 'error': ''},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='New directory for widget screenshots')
    parser.add_argument('--empty', action='store_true', help='Show the phone-free empty states')
    parser.add_argument('--untimed', action='store_true', help='Track text and controls without artwork/timing, as on the bench')
    parser.add_argument('--long-text', action='store_true', help='Maximum-length track/artist/phone labels')
    parser.add_argument('--home-only', action='store_true', help='Capture only Home, with its actual widget bounds')
    parser.add_argument('--all-tabs', action='store_true', help='Include configured Camera and Settings in the dock')
    parser.add_argument('--portrait', action='store_true', help='Request a 720 by 1280 portrait window')
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    if args.empty:
        STATUS.update(phones=[], active=None, active_mode=None, ultra_cluster=None,
                      carplay={'state': 'disconnected', 'returnReady': False},
                      media={'available': False, 'controls': []})
    elif args.untimed:
        STATUS['media'].update(title="Don't Forget Me", artist='Red Hot Chili Peppers',
                               status='paused', controls=['previous', 'play', 'next'])
        STATUS['media'].pop('position_ms', None)
        STATUS['media'].pop('duration_ms', None)
    if args.long_text:
        STATUS['media'].update(title='W' * 2048, artist='W' * 2048)
        if STATUS['phones']:
            STATUS['phones'][0]['name'] = 'W' * 248
    if args.all_tabs:
        STATUS['display_response'] = {'configured': True, 'mode': 'balanced'}

    def fixture_request(_path, command):
        if command != {'action': 'status'}:
            raise ValueError('This design preview is read-only.')
        return copy.deepcopy(STATUS)

    phone_page.request = fixture_request
    app = phone_page.PhonePage(Path('/unopened/native-home-design.sock'))
    app.set_flags(Gio.ApplicationFlags.NON_UNIQUE)
    if args.portrait:
        app.connect_after('activate', lambda _: app.window.set_default_size(720, 1280))
    view_names = ['home'] if args.home_only else ['home', 'phones', 'music', 'cluster', 'camera']
    if args.all_tabs and not args.home_only:
        view_names.append('settings')
    views = iter(view_names)
    current = None
    changed_at = None
    started = time.monotonic()
    errors = []
    bounds = {}
    navigation = {}
    dock_geometry = {}

    def tick():
        nonlocal current, changed_at
        try:
            if time.monotonic() - started > 15:
                raise RuntimeError('Design preview timed out')
            if not hasattr(app, 'pages') or app.pending or app.rows_key is None:
                return True
            if current is None:
                current = next(views, None)
                if current is None:
                    app.quit()
                    return False
                app.view_buttons[current].set_active(True)
                changed_at = time.monotonic()
                return True
            if app.pages.get_transition_running() or time.monotonic() - changed_at < .5:
                return True
            paintable = Gtk.WidgetPaintable.new(app.window)
            snapshot = Gtk.Snapshot.new()
            paintable.snapshot(snapshot, app.window.get_width(), app.window.get_height())
            texture = app.window.get_renderer().render_texture(snapshot.to_node(), None)
            if not texture.save_to_png(str(destination / (current + '.png'))):
                raise RuntimeError('Could not save preview')
            tabs = {}
            for name, button in app.view_buttons.items():
                if not button.get_visible():
                    continue
                ok, area = button.compute_bounds(app.window)
                if not ok:
                    raise RuntimeError('Missing dock allocation: ' + name)
                x, y, w, h = area.get_x(), area.get_y(), area.get_width(), area.get_height()
                if min(x, y) < 0 or x + w > app.window.get_width() or y + h > app.window.get_height():
                    raise RuntimeError('Dock target outside the viewport: ' + name)
                if min(w, h) < 44:
                    raise RuntimeError('Dock target too small: ' + name)
                tabs[name] = {'x': x, 'y': y, 'width': w, 'height': h}
            navigation[current] = tabs
            ok, area = app.tab_bar.compute_bounds(app.window)
            if not ok:
                raise RuntimeError('Missing dock surface allocation')
            outer = (area.get_x(), area.get_y(), area.get_width(), area.get_height())
            radius = min(app.tab_bar.painter.radius, outer[2] / 2, outer[3] / 2)
            first, last = next(iter(tabs)), next(reversed(tabs))
            if current in (first, last):
                tab = tabs[current]
                inner_radius = min(tab['width'], tab['height']) / 2
                # The rim and the end selection must have concentric corners.
                vertical = app.tab_bar.get_orientation() == Gtk.Orientation.VERTICAL
                cross_start, cross_size = ('x', 'width') if vertical else ('y', 'height')
                axis_start, axis_size = ('y', 'height') if vertical else ('x', 'width')
                c, a = (0, 1) if vertical else (1, 0)
                centres = [(outer[c] + radius, tab[cross_start] + inner_radius),
                           (outer[c] + outer[c + 2] - radius,
                            tab[cross_start] + tab[cross_size] - inner_radius)]
                if current == first:
                    centres.append((outer[a] + radius, tab[axis_start] + inner_radius))
                if current == last:
                    centres.append((outer[a] + outer[a + 2] - radius,
                                    tab[axis_start] + tab[axis_size] - inner_radius))
                if any(abs(a - b) > 1 for a, b in centres):
                    raise RuntimeError('Dock and end selection corners do not nest: ' + current)
            dock_geometry[current] = {'surface': outer, 'radius': radius}
            if current == 'home' and not args.portrait:
                for name, widget in [('shortcuts', app.home.apps), ('player', app.home.player),
                                     ('clock', app.home.clock), ('date', app.home.date),
                                     ('phoneAction', app.home.phone_button),
                                     *[('app_' + key, value) for key, value in app.home.app_buttons.items()]]:
                    ok, area = widget.compute_bounds(app.window)
                    if not ok:
                        raise RuntimeError('Missing allocation for ' + name)
                    x, y, w, h = area.get_x(), area.get_y(), area.get_width(), area.get_height()
                    if min(x, y) < 0 or x + w > app.window.get_width() + 1 or y + h > app.window.get_height() + 1:
                        raise RuntimeError('Home element outside the viewport: ' + name)
                    bounds[name] = {'x': x, 'y': y, 'width': w, 'height': h}
                a, b = bounds['shortcuts'], bounds['player']
                if abs(a['y'] - b['y']) > 1 or abs(a['height'] - b['height']) > 1:
                    raise RuntimeError('Home columns do not share top/bottom edges')
                if abs(bounds['clock']['x'] - a['x']) > 1 or abs(
                        bounds['phoneAction']['x'] + bounds['phoneAction']['width'] - b['x'] - b['width']) > 1:
                    raise RuntimeError('Home header does not align with the content grid')
                rows = [bounds['app_' + key] for key in ('maps', 'music', 'phone')]
                if max(row['height'] for row in rows) - min(row['height'] for row in rows) > 1:
                    raise RuntimeError('App touch targets have unequal heights')
            current = None
            return True
        except Exception as error:
            errors.append(str(error))
            app.quit()
            return False

    GLib.timeout_add(40, tick)
    app.run([])
    if errors:
        raise RuntimeError('; '.join(errors))
    def resolved_font(widget):
        context = widget.get_pango_context()
        return context.load_font(context.get_font_description()).describe().get_family()

    (destination / 'evidence.json').write_text(json.dumps({
        'scope': 'Actual GTK widget screenshots with read-only synthetic metadata',
        'views': view_names,
        'orientation': 'portrait' if args.portrait else 'landscape',
        'windowSize': [app.window.get_width(), app.window.get_height()],
        'phoneHardware': False, 'physicalDisplay': False,
        'state': 'empty' if args.empty else ('untimed' if args.untimed else 'connected'),
        'fonts': {'heading': resolved_font(app.page_title), 'body': resolved_font(app.page_subtitle)},
        'homeBounds': bounds,
        'navigationBounds': navigation,
        'dockGeometry': dock_geometry,
    }, indent=2) + '\n')
    print('Native Home design previews:', destination)


if __name__ == '__main__':
    main()
