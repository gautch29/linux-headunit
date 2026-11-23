# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Home dashboard and settings sections: clock, backdrop, widget and preference states; no manager."""
from pathlib import Path
import struct
import sys
import time
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_glass import AdaptiveShell, corner_offset, ease, GlassTabBar, rect, shape_path  # noqa: E402
from phone_home import (AMBIENT_SIZE, artwork_ambient, Backdrop, clock_strings, ClusterFormats,  # noqa: E402
                        CarView, DriveModeSettings, GreetingSettings, HomeView, orb_palette, vehicle_value)
from gi.repository import Graphene, Gtk  # noqa: E402


def png(width, height, colour):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\x00' + bytes(colour) * width for _ in range(height))
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


class Owner:
    def __init__(self):
        self.returnable = False
        self.media = []
        self.actions = []
        self.view_buttons = {'music': Gtk.ToggleButton(), 'phones': Gtk.ToggleButton()}

    def can_return(self):
        return self.returnable

    def media_action(self, command):
        self.media.append(command)

    def open_phone_app(self, app):
        pass

    def back(self):
        pass

    def action(self, value):
        self.actions.append(value)


class ClockTest(unittest.TestCase):
    def test_unset_wall_clock_is_unavailable(self):
        self.assertIsNone(clock_strings(0))

    def test_set_wall_clock_formats_time_and_date(self):
        moment = time.mktime((2026, 10, 13, 22, 5, 0, 0, 0, -1))
        self.assertEqual(clock_strings(moment), ('22:05', 'Tuesday 13 October'))


class BackdropTest(unittest.TestCase):
    def test_artwork_becomes_a_small_vivid_texture(self):
        texture = artwork_ambient(png(64, 64, (200, 40, 90)))
        self.assertEqual((texture.get_width(), texture.get_height()), (AMBIENT_SIZE, AMBIENT_SIZE))

    def test_undecodable_artwork_is_rejected(self):
        self.assertIsNone(artwork_ambient(b'\x89PNG\r\n\x1a\nbroken'))

    def test_artwork_and_visibility_invalidate_observing_glass_surfaces(self):
        class Surface:
            redraws = 0

            def queue_draw(self):
                self.redraws += 1

        Gtk.init()
        backdrop = Backdrop()
        surface = Surface()
        backdrop.watch_glass(surface)
        backdrop.set_texture(backdrop.default)
        self.assertEqual((surface.redraws, backdrop.revision), (1, 1))
        backdrop.widget.set_visible(False)
        self.assertEqual(surface.redraws, 2)
        backdrop.widget.set_visible(True)
        self.assertEqual(surface.redraws, 3)


class HomeViewTest(unittest.TestCase):
    def setUp(self):
        Gtk.init()
        self.owner = Owner()
        self.home = HomeView(self.owner)
        self.phone = {'address': 'AA:BB:CC:DD:EE:01', 'name': 'Bench phone'}

    def test_unset_clock_hides_heading(self):
        self.home.tick(0)
        self.assertFalse(self.home.clock.get_visible())
        self.assertFalse(self.home.date.get_visible())

    def test_missing_media_shows_no_transport_or_progress(self):
        self.home.render({'media': {'available': False}}, None, None)
        self.assertEqual(self.home.title.get_text(), 'Nothing playing')
        self.assertFalse(self.home.transport.get_visible())
        self.assertFalse(self.home.progress.get_visible())

    def test_playing_media_offers_pause_not_play(self):
        media = {'available': True, 'title': 'Song', 'artist': 'Artist', 'status': 'playing',
                 'position_ms': 30000, 'duration_ms': 120000, 'controls': ['play', 'pause', 'next']}
        self.home.render({'media': media}, 'ready', None)
        self.assertTrue(self.home.buttons['pause'].get_visible())
        self.assertFalse(self.home.buttons['play'].get_visible())
        self.assertFalse(self.home.buttons['previous'].get_visible())
        self.assertAlmostEqual(self.home.progress.get_fraction(), 0.25)
        self.assertEqual(self.home.elapsed.get_text(), '0:30')
        self.assertEqual(self.home.remaining.get_text(), '−1:30')

    def test_unknown_position_hides_progress(self):
        media = {'available': True, 'title': 'Song', 'duration_ms': 120000, 'controls': []}
        self.home.render({'media': media}, 'ready', None)
        self.assertFalse(self.home.progress.get_visible())

    def test_observed_position_past_end_does_not_show_negative_remaining_time(self):
        media = {'available': True, 'position_ms': 130000, 'duration_ms': 120000}
        self.home.render({'media': media}, 'ready', None)
        self.assertEqual(self.home.remaining.get_text(), '−0:00')
        self.assertEqual(self.home.progress.get_fraction(), 1)

    def test_live_ultra_phone_offers_carplay_when_returnable(self):
        self.owner.returnable = True
        data = {'media': {}, 'active': self.phone['address'], 'active_mode': 'ultra', 'phones': [self.phone]}
        self.home.render(data, 'ready', None)
        self.assertEqual(self.home.phone_state.get_text(), 'CarPlay Ultra')
        self.assertTrue(self.home.phone_dot.has_css_class('live'))
        self.assertEqual(self.home.phone_button.get_label(), 'Open CarPlay')

    def test_starting_session_is_not_live(self):
        data = {'media': {}, 'active': self.phone['address'], 'active_mode': 'ultra', 'phones': [self.phone]}
        self.home.render(data, 'starting', None)
        self.assertEqual(self.home.phone_state.get_text(), 'Connecting…')
        self.assertFalse(self.home.phone_dot.has_css_class('live'))
        self.assertEqual(self.home.phone_button.get_label(), 'Phones')

    def test_lost_manager_retires_shortcuts(self):
        self.home.set_shortcuts(True)
        self.home.lost()
        self.assertTrue(self.home.apps.get_visible())
        self.assertFalse(any(button.get_sensitive() for button in self.home.app_buttons.values()))
        self.assertEqual(self.home.phone_name.get_text(), 'Phone status unavailable')


class GlassTest(unittest.TestCase):
    def setUp(self):
        Gtk.init()

    def test_spring_starts_and_settles(self):
        self.assertAlmostEqual(ease(0), 0)
        self.assertAlmostEqual(ease(1), 1)
        self.assertGreater(max(ease(t / 20) for t in range(21)), 1.0)

    def test_pill_path_is_built_for_any_radius(self):
        area = Graphene.Rect().init(0, 0, 96, 400)
        self.assertIsNotNone(shape_path(area, 999))
        self.assertIsNotNone(shape_path(area, 0))

    def test_first_selection_does_not_animate(self):
        bar = GlassTabBar(Backdrop())
        button = Gtk.ToggleButton()
        bar.append(button)
        bar.select(button)
        self.assertEqual(bar.tick_id, 0)

    def test_shell_places_bar_left_in_landscape_and_below_in_portrait(self):
        bar = GlassTabBar(Backdrop())
        content = Gtk.Box()
        shell = AdaptiveShell(bar, content)
        self.assertIs(shell.get_first_child(), bar)
        self.assertEqual(bar.get_orientation(), Gtk.Orientation.VERTICAL)
        shell.apply(False)
        self.assertIs(shell.get_first_child(), content)
        self.assertEqual(bar.get_orientation(), Gtk.Orientation.HORIZONTAL)
        self.assertEqual(shell.get_orientation(), Gtk.Orientation.VERTICAL)


def list_group():
    return Gtk.FlowBox()


def list_row(button):
    return button


MODES = [{'id': 'comfort', 'name': 'GT'}, {'id': 'track', 'name': 'Track'}]


class SettingsTest(unittest.TestCase):
    def setUp(self):
        self.owner = Owner()

    def test_cluster_formats_offer_only_available_pages(self):
        formats = ClusterFormats(self.owner, list_group, list_row)
        formats.render(('media', 'trip', 'sport'), 'sport')
        self.assertTrue(formats.previews['sport'].selected)
        self.assertFalse(formats.buttons['minimal'].get_visible())
        self.assertFalse(formats.buttons['map'].get_parent().get_visible())
        self.assertFalse(formats.marks['media'].get_visible())
        formats.buttons['trip'].emit('clicked')
        self.assertEqual(self.owner.actions, [{'action': 'cluster_page', 'page': 'trip'}])

    def test_lost_status_unmarks_formats(self):
        formats = ClusterFormats(self.owner, list_group, list_row)
        formats.render(('media', 'gauges'), 'gauges')
        formats.unmark()
        self.assertFalse(formats.previews['gauges'].selected)
        self.assertFalse(formats.buttons['gauges'].has_css_class('selected'))

    def test_unconfigured_drive_modes_are_hidden(self):
        drive = DriveModeSettings(self.owner)
        self.assertFalse(drive.render({'configured': False, 'modes': []}))
        self.assertFalse(drive.widget.get_visible())

    def test_drive_mode_choice_carries_current_revision(self):
        drive = DriveModeSettings(self.owner)
        self.assertTrue(drive.render({'configured': True, 'revision': 7, 'selected': None, 'modes': MODES,
                                      'current': None, 'error': ''}))
        self.assertTrue(drive.buttons[None].has_css_class('selected'))
        drive.buttons['track'].emit('clicked')
        self.assertEqual(self.owner.actions, [{'action': 'drive_mode', 'mode': 'track', 'revision': 7}])

    def test_requested_drive_mode_is_not_shown_as_applied(self):
        drive = DriveModeSettings(self.owner)
        drive.render({'configured': True, 'revision': 2, 'selected': 'track', 'modes': MODES,
                      'current': {'source': 'user', 'mode': 'track', 'phase': 'pending', 'error': ''}, 'error': ''})
        self.assertEqual(drive.state.get_text(), 'Applying to the cluster…')
        drive.render({'configured': True, 'revision': 2, 'selected': 'track', 'modes': MODES,
                      'current': None, 'error': ''})
        self.assertEqual(drive.state.get_text(), 'Saved. Waiting for the cluster.')

    def test_observed_car_mode_is_attributed_to_the_car(self):
        drive = DriveModeSettings(self.owner)
        drive.render({'configured': True, 'revision': 2, 'selected': None, 'modes': MODES,
                      'current': {'source': 'vehicle', 'mode': 'comfort', 'phase': 'confirmed', 'error': ''}, 'error': ''})
        self.assertEqual(drive.state.get_text(), 'Car reports GT · Shown on the cluster')

    def test_greeting_previews_only_for_an_animation(self):
        greeting = GreetingSettings(self.owner)
        self.assertFalse(greeting.render({'configured': False}))
        greeting.render({'configured': True, 'variant': 'off'})
        self.assertFalse(greeting.previews['startup'].get_visible())
        greeting.render({'configured': True, 'variant': 'minimal'})
        self.assertTrue(greeting.buttons['minimal'].has_css_class('selected'))
        greeting.previews['goodbye'].emit('clicked')
        greeting.buttons['brand'].emit('clicked')
        self.assertEqual(self.owner.actions, [{'action': 'greeting_preview', 'phase': 'goodbye'},
                                              {'action': 'greeting_sequence', 'variant': 'brand'}])


def car_rows(**states):
    ids = ('fuel_percent', 'range_km', 'battery_voltage', 'ambient_c', 'coolant_c', 'oil_c', 'oil_pressure_kpa', 'odometer_km')
    rows = []
    for identifier in ids:
        availability, value, origin = states.get(identifier, ('available', 1, 'measured'))
        rows.append({'id': identifier, 'availability': availability, 'value': value, 'origin': origin})
    return rows


class CarViewTest(unittest.TestCase):
    def test_unconfigured_car_page_is_not_offered(self):
        car = CarView(list_group, list_row)
        self.assertFalse(car.render({'configured': False, 'state': 'disabled'}))
        self.assertFalse(car.render(None))

    def test_values_use_display_units_and_keep_real_zero(self):
        self.assertEqual(vehicle_value('oil_pressure_kpa', 320), '3.2 bar')
        self.assertEqual(vehicle_value('ambient_c', -4.4), '-4°C')
        self.assertEqual(vehicle_value('fuel_percent', 0), '0%')
        self.assertIsNone(vehicle_value('range_km', None))

    def test_missing_and_stale_readings_stay_distinct(self):
        car = CarView(list_group, list_row)
        self.assertTrue(car.render({'configured': True, 'state': 'admitted', 'rows': car_rows(
            fuel_percent=('stale', None, 'measured'), odometer_km=('missing', None, None),
            oil_c=('unavailable', None, 'measured'))}))
        self.assertEqual(car.values['fuel_percent'].get_text(), '—')
        self.assertEqual(car.states['fuel_percent'].get_text(), 'Out of date')
        self.assertFalse(car.fuel.get_visible())
        self.assertEqual(car.values['odometer_km'].get_text(), 'Not reported')
        self.assertEqual(car.values['oil_c'].get_text(), 'Unavailable')
        self.assertFalse(car.unavailable.get_visible())
        self.assertFalse(car.simulated.get_visible())

    def test_simulated_readings_are_labelled(self):
        car = CarView(list_group, list_row)
        car.render({'configured': True, 'state': 'admitted', 'rows': car_rows(range_km=('available', 420, 'simulation'))})
        self.assertTrue(car.simulated.get_visible())
        self.assertEqual(car.values['range_km'].get_text(), '420 km')

    def test_failed_publication_shows_no_readings(self):
        car = CarView(list_group, list_row)
        self.assertTrue(car.render({'configured': True, 'state': 'publication_unavailable'}))
        self.assertTrue(car.unavailable.get_visible())
        self.assertEqual(car.reason.get_text(), 'The car data service could not start.')
        self.assertEqual(car.values['coolant_c'].get_text(), '—')

    def test_lost_status_forgets_readings(self):
        car = CarView(list_group, list_row)
        car.render({'configured': True, 'state': 'admitted', 'rows': car_rows()})
        car.lost()
        self.assertEqual(car.values['range_km'].get_text(), '—')
        self.assertTrue(car.unavailable.get_visible())


class WallpaperTest(unittest.TestCase):
    def test_palette_picks_distinct_vivid_colours(self):
        cells = bytes((200, 30, 60) * 20 + (20, 20, 20) * 10 + (30, 60, 210) * 6)
        palette = orb_palette(cells)
        self.assertEqual(len(palette), 3)
        # The two vivid colours lead; the grey cells are not chosen over them.
        self.assertEqual({max(range(3), key=colour.__getitem__) for colour in palette[:2]}, {0, 2})

    def test_grey_artwork_still_yields_colours(self):
        palette = orb_palette(bytes((90, 90, 90) * 36))
        self.assertEqual(len(palette), 3)
        self.assertTrue(all(0 <= channel <= 1 for colour in palette for channel in colour))


class SpecularTest(unittest.TestCase):
    def test_corner_directions_follow_the_shape(self):
        square = rect(0, 0, 100, 100)
        self.assertAlmostEqual(corner_offset(square, -1, -1), 0.875)
        self.assertAlmostEqual(corner_offset(square, 1, 1), 0.375)
        wide = rect(0, 0, 400, 100)
        # A wide card's corners sit nearer the horizontal axis.
        self.assertGreater(corner_offset(wide, 1, 1), 0.25)
        self.assertLess(corner_offset(wide, 1, 1), 0.375)


if __name__ == '__main__':
    unittest.main()
