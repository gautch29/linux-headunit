# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""System BlueZ adapter. All operations target a validated paired Device1."""
from phone_state import address


class BluezPhones:
    def __init__(self):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)

    def call(self, path, interface, method, signature=None, values=()):
        result = self.bus.call_sync('org.bluez', path, interface, method,
                                   self.GLib.Variant(signature, values) if signature else None,
                                   None, self.Gio.DBusCallFlags.NONE, 5000, None)
        return result.unpack()

    def objects(self):
        return self.call('/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects')[0]

    def phones(self):
        phones = []
        for path, interfaces in self.objects().items():
            p = interfaces.get('org.bluez.Device1', {})
            if not p.get('Paired'):
                continue
            # Include paired devices exposing iAP, phone icon/class, or previously
            # recognized iPhones. Do not offer removal of keyboards/touch devices.
            uuids = [u.lower() for u in p.get('UUIDs', [])]
            if not (p.get('Icon') == 'phone' or (p.get('Class', 0) >> 8) & 31 == 2 or
                    '00000000-deca-fade-deca-deafdecacafe' in uuids):
                continue
            phones.append({'address': address(p['Address']), 'name': p.get('Alias', p.get('Name', 'Phone'))[:128],
                           'connected': bool(p.get('Connected')), 'path': path})
        return sorted(phones, key=lambda p: (p['name'].casefold(), p['address']))

    def phone(self, target):
        target = address(target)
        for phone in self.phones():
            if phone['address'] == target:
                return phone
        raise ValueError('Phone is no longer paired')

    def disconnect(self, target):
        p = self.phone(target)
        if p['connected']:
            self.call(p['path'], 'org.bluez.Device1', 'Disconnect')

    def forget(self, target):
        p = self.phone(target)
        self.call(p['path'].rsplit('/', 1)[0], 'org.bluez.Adapter1', 'RemoveDevice', '(o)', (p['path'],))

    def media(self, target):
        """No cached cross-phone metadata; absence is explicitly unavailable."""
        p = self.phone(target)
        if not p['connected']:
            return {'available': False, 'controls': []}
        objects = self.objects()
        player = objects.get(p['path'], {}).get('org.bluez.MediaControl1', {}).get('Player')
        data = objects.get(player, {}).get('org.bluez.MediaPlayer1', {}) if player else {}
        if not data:
            return {'available': False, 'controls': []}
        track = data.get('Track', {})
        result = {'available': True, 'source': 'bluetooth', 'title': str(track.get('Title', ''))[:512],
                  'artist': str(track.get('Artist', ''))[:512], 'album': str(track.get('Album', ''))[:512],
                  'genre': str(track.get('Genre', ''))[:256], 'duration_ms': track.get('Duration'),
                  'position_ms': data.get('Position'), 'track_number': track.get('TrackNumber'),
                  'track_count': track.get('NumberOfTracks'), 'player': data.get('Name', ''),
                  'status': data.get('Status', 'unknown'), 'artwork': None,
                  'controls': ['play', 'pause', 'next', 'previous']}
        for path, interfaces in objects.items():
            transport = interfaces.get('org.bluez.MediaTransport1', {})
            if transport.get('Device') == p['path'] and 'Volume' in transport:
                result['volume'] = transport['Volume'] / 127
                result['controls'] += ['volume_up', 'volume_down']
                break
        return result

    def media_command(self, target, command):
        p = self.phone(target)
        if not p['connected']:
            raise ValueError('Phone is disconnected')
        objects = self.objects()
        control = objects.get(p['path'], {}).get('org.bluez.MediaControl1', {})
        player = control.get('Player')
        methods = {'play': 'Play', 'pause': 'Pause', 'next': 'Next', 'previous': 'Previous'}
        if command in methods and player and 'org.bluez.MediaPlayer1' in objects.get(player, {}):
            self.call(player, 'org.bluez.MediaPlayer1', methods[command])
            return
        if command in ('volume_up', 'volume_down'):
            for path, interfaces in objects.items():
                t = interfaces.get('org.bluez.MediaTransport1', {})
                if t.get('Device') == p['path'] and 'Volume' in t:
                    value = max(0, min(127, t['Volume'] + (5 if command == 'volume_up' else -5)))
                    self.call(path, 'org.freedesktop.DBus.Properties', 'Set', '(ssv)',
                              ('org.bluez.MediaTransport1', 'Volume', self.GLib.Variant('q', value)))
                    return
        raise ValueError('This media control is unavailable for the connected phone')
