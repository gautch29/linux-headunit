# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real GTK artwork admission/cache fixture; no manager, phone or Bluetooth."""
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import phone_page
from phone_page import PhonePage, Gtk, GLib


def png(width, height, color):
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    pixels = (b'\0' + bytes(color) * width) * height
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')


class ArtworkPage(PhonePage):
    def refresh(self):
        # This fixture exercises the native consumer directly. It must not
        # open a real manager socket or manufacture a receiver-ready reply.
        return True


os.umask(0o077)
with tempfile.TemporaryDirectory(prefix='headunit-native-artwork-') as directory:
    path = Path(directory) / 'media.cover.png'
    app = ArtworkPage(Path(directory) / 'manager.sock')
    errors, stage = [], 0
    started = time.monotonic()
    first, second = png(3, 2, (180, 30, 40)), png(4, 2, (20, 140, 80))
    media = {'available': True, 'title': 'Original track', 'artist': 'Fixture artist',
             'status': 'paused', 'controls': []}
    reply = {'phones': [], 'active': None, 'active_mode': None, 'cluster_pages': ['media', 'vehicle', 'trip'],
             'cluster_page': 'media', 'media': media}

    def publish(raw):
        temporary = path.with_suffix('.tmp')
        temporary.write_bytes(raw)
        temporary.chmod(0o600)
        os.replace(temporary, path)

    def render(raw=None, **changes):
        if raw is not None:
            publish(raw)
            media.update(artwork=path.name, artwork_id=hashlib.sha256(raw).hexdigest())
        media.update(changes)
        app.render(reply)

    def empty():
        assert app.cover_stack.get_visible_child_name() == 'empty'
        assert app.cover.get_paintable() is None and app.artwork_cache is None
        assert app.song.get_text() == media['title']  # Cover faults do not erase valid text.

    def admitted(width):
        texture = app.cover.get_paintable()
        assert app.cover_stack.get_visible_child_name() == 'art'
        assert texture.get_width() == width and texture.get_height() == 2
        assert app.artwork_cache[1] is texture
        return texture

    def checks():
        real_open, reads = os.open, []

        def counted_open(*args, **kwargs):
            if Path(args[0]) == path:
                reads.append(path)
            return real_open(*args, **kwargs)

        with patch.object(phone_page.os, 'open', counted_open):
            render(first)
            texture = admitted(3)
            for _ in range(25):
                render()
                assert admitted(3) is texture
            assert len(reads) == 1, 'Unchanged verified art was reopened or decoded'

            # New cover bytes can arrive before the matching manager reply.
            # Reject successor art against old metadata, then recover exactly.
            publish(second)
            render(); empty()
            render(artwork_id=hashlib.sha256(second).hexdigest(), title='Successor track')
            assert admitted(4) is not texture

            # Matching digest is necessary but malformed/oversized PNG data is
            # still rejected before it can reach an unbounded image decoder.
            real_decode, decode_errors = phone_page.Gdk.Texture.new_from_bytes, []

            def observed_decode(*args):
                try:
                    return real_decode(*args)
                except GLib.Error as error:
                    decode_errors.append(error)
                    raise

            with patch.object(phone_page.Gdk.Texture, 'new_from_bytes', observed_decode):
                for raw in (second[:33], png(1025, 2, (0, 0, 0)), b'x' * 1048577):
                    render(raw); empty()
                    render(first); admitted(3)
            assert len(decode_errors) == 1, 'Truncated PNG did not exercise actual Gdk decode failure'

            for digest in ('', 'x' * 64, 'a' * 64):
                render(artwork_id=digest); empty()
            render(first); admitted(3)
            path.unlink(); render(); empty()
            render(first); admitted(3)
            render(available=False); empty()
            render(available=True); admitted(3)

        for unsafe in ('mode', 'symlink', 'hardlink', 'fifo', 'directory'):
            render(first)
            other = Path(directory) / 'other.png'
            if unsafe == 'mode':
                path.chmod(0o644)
            elif unsafe == 'hardlink':
                os.link(path, other)
            else:
                path.unlink()
                if unsafe == 'symlink':
                    other.write_bytes(first); path.symlink_to(other)
                elif unsafe == 'fifo':
                    os.mkfifo(path, 0o600)
                else:
                    path.mkdir()
            render(); empty()
            path.rmdir() if path.is_dir() else path.unlink()
            if other.exists():
                other.unlink()

        # Retire the pinned inode during open. No subsequent path reopen may
        # decode its successor under the original digest.
        render(first); media['artwork_id'] = hashlib.sha256(second).hexdigest()
        replaced = False

        def replace_after_open(*args, **kwargs):
            nonlocal replaced
            descriptor = real_open(*args, **kwargs)
            if Path(args[0]) == path and not replaced:
                replaced = True
                publish(second)
            return descriptor

        with patch.object(phone_page.os, 'open', replace_after_open):
            render(); empty()
        assert replaced
        render(); admitted(4)

        # A hung manager must retire the entire dated presentation, even though
        # coherent artwork and the last active camera/audio reply still exist.
        media.update(position_ms=120000, duration_ms=600000, controls=['pause'])
        reply.update(active='AA:BB:CC:DD:EE:01',
                     carplay={'state': 'ready', 'returnReady': True},
                     camera={'configured': True, 'requested': True, 'phase': 'active', 'error': ''},
                     audio={'available': True, 'volumePercent': 70, 'muted': False, 'pending': False, 'error': ''})
        app.received('status', dict(reply, ok=True), observed_at=time.monotonic())
        admitted(4)
        assert app.song.get_text() == media['title'] and app.progress.get_fraction() == .2
        assert app.audio_value.get_text() == '70%' and app.camera_status.get_text() == 'Camera view visible'
        assert app.camera_request_status.get_text() == 'Manual request · On'
        app.status_requested_at = time.monotonic() - 5.1
        assert app.health_tick()
        assert not app.status_current() and not app.camera_controls.get_sensitive()
        assert app.cover_stack.get_visible_child_name() == 'empty'
        assert app.cover.get_paintable() is None and app.artwork_cache is None
        assert app.song.get_text() == 'Media information unavailable'
        assert not app.media_note.get_visible()
        assert app.album.get_text() == '' and not app.album.get_visible()
        assert app.elapsed.get_text() == app.duration.get_text() == '—:—'
        assert not app.timeline.get_visible() and app.progress.get_fraction() == 0
        assert not any(button.is_sensitive() for button in app.buttons.values())
        assert app.audio_value.get_text() == 'Unavailable' and not app.audio_route_controls.get_visible()
        assert app.camera_status.get_text() == 'Camera information unavailable'
        assert app.camera_request_status.get_text() == 'Manual request · Unavailable'
        assert app.retry_button.get_visible() and app.retry_button.get_sensitive()
        app.received('status', dict(reply, ok=True), observed_at=time.monotonic())
        admitted(4)
        assert app.song.get_text() == media['title'] and app.audio_value.get_text() == '70%'
        assert app.camera_request_status.get_text() == 'Manual request · On'
        assert not app.retry_button.get_visible()
        for bad in ({'configured': 'yes', 'requested': False, 'phase': 'active', 'error': ''},
                    {'configured': True, 'requested': False, 'phase': 'future', 'error': ''},
                    ['active']):
            app.received('status', dict(reply, ok=True, camera=bad), observed_at=time.monotonic())
            assert not app.camera_controls.get_sensitive()
            assert not app.view_buttons['camera'].get_visible()
            assert app.camera_request_status.get_text() == 'Manual request · Unavailable'
        app.received('status', dict(reply, ok=True), observed_at=time.monotonic())

    def tick():
        global stage
        try:
            assert time.monotonic() - started < 8, 'Native artwork fixture exceeded its bound'
            if not hasattr(app, 'cover') or app.window.get_width() == 0:
                return True
            if stage == 0:
                app.view_buttons['music'].set_active(True)
                checks()
                stage = 1
                return True
            if app.pages.get_transition_running():
                return True
            if destination := os.environ.get('HEADUNIT_ARTWORK_SCREENSHOT'):
                paintable = Gtk.WidgetPaintable.new(app.window)
                snapshot = Gtk.Snapshot.new()
                paintable.snapshot(snapshot, app.window.get_width(), app.window.get_height())
                texture = app.window.get_renderer().render_texture(snapshot.to_node(), None)
                assert texture.save_to_png(destination)
            app.quit()
        except Exception as error:
            errors.append(error); app.quit()
        return False

    GLib.timeout_add(50, tick)
    app.run([])
    app.close_ui()
    if errors:
        raise errors[0]
    print('Native GTK artwork: exact publication bytes, cache reuse, actual Gdk truncated-PNG exception, corruption/unsafe-file rejection, atomic replacement, expired-manager camera/media/audio retirement, malformed camera rejection and fresh recovery passed; no phone or deployment')
