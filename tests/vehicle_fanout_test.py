# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import errno
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from vehicle_fanout import (Fanout, MAX_PACKET, MAX_LEGACY_PACKET, MAX_QUEUE_AGE_NS, TIMESTAMPNS,
                           TIMESPEC, ingress_packet, load_configuration, private_parent, installed_ui_source)


class InstalledUiSourceTests(unittest.TestCase):
    PROFILE = 'simulateGauges=false\nvehicleSocket=/private/ultra-one.sock\n'

    def test_regular_profile_and_complete_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'installed-ui.conf'
            path.write_text(self.PROFILE)
            path.chmod(0o600)
            expected = {'simulateGauges': 'false', 'vehicleSocket': '/private/ultra-one.sock'}
            self.assertEqual(installed_ui_source(path, os.getuid()), expected)
            replacement = path.with_suffix('.pending')
            replacement.write_text(self.PROFILE)
            replacement.chmod(0o444)  # Retain admission of sealed read-only profiles.
            os.replace(replacement, path)
            self.assertEqual(installed_ui_source(path, os.getuid()), expected)
            path.chmod(0o600)
            bounded = self.PROFILE + '#' + 'x' * (8192 - len(self.PROFILE) - 1)
            path.write_text(bounded)
            self.assertEqual(path.stat().st_size, 8192)
            self.assertEqual(installed_ui_source(path, os.getuid()), expected)

    def test_unsafe_type_link_permissions_and_size_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / 'installed-ui.conf'
            path.write_text(self.PROFILE)
            path.chmod(0o600)
            link = root / 'link'
            link.symlink_to(path)
            oversized = root / 'oversized'
            oversized.write_bytes(b'x' * 8193)
            for invalid in (root, link, oversized):
                with self.subTest(path=invalid), self.assertRaises((OSError, ValueError)):
                    installed_ui_source(invalid, os.getuid())
            path.chmod(0o622)
            with self.assertRaises(ValueError):
                installed_ui_source(path, os.getuid())

    def test_unsupported_characters_remain_rejected(self):
        profiles = (self.PROFILE + '# hidden\0data\n',
                    self.PROFILE.replace('\n', '\r\n'),
                    self.PROFILE.replace('ultra-one', 'ultra\0-one'),
                    self.PROFILE + '# hidden\rdata\n')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'installed-ui.conf'
            for profile in profiles:
                with self.subTest(profile=profile):
                    path.write_bytes(profile.encode())
                    path.chmod(0o600)
                    with self.assertRaisesRegex(ValueError, 'unsupported characters'):
                        installed_ui_source(path, os.getuid())

    def test_symlink_or_permission_change_during_read_rejected(self):
        for change in ('symlink', 'permission'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'installed-ui.conf'
                path.write_text(self.PROFILE)
                path.chmod(0o600)
                original_read = os.read

                def mutate_after_read(fd, length):
                    raw = original_read(fd, length)
                    if change == 'symlink':
                        target = path.with_suffix('.target')
                        target.write_text(self.PROFILE)
                        target.chmod(0o600)
                        path.unlink()
                        path.symlink_to(target)
                    else:
                        path.chmod(0o666)
                    return raw

                with patch('vehicle_fanout.os.read', mutate_after_read):
                    with self.assertRaisesRegex(ValueError, 'changed while reading'):
                        installed_ui_source(path, os.getuid())

    def test_fifo_process_rejects_without_waiting_for_writer(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fifo'
            os.mkfifo(path, 0o600)
            source = Path(__file__).resolve().parents[1] / 'tools'
            code = ("import os,sys; sys.path.insert(0,sys.argv[1]); "
                    "from vehicle_fanout import installed_ui_source; "
                    "installed_ui_source(sys.argv[2],os.getuid())")
            try:
                result = subprocess.run([sys.executable, '-B', '-c', code, str(source), str(path)],
                                        capture_output=True, timeout=2)
            except subprocess.TimeoutExpired:
                self.fail('Installed UI source FIFO blocked validation until the process deadline')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'bounded owned regular file', result.stderr)

    def test_mutation_with_restored_mtime_and_retired_inode_rejected(self):
        for atomic in (False, True):
            with self.subTest(atomic=atomic), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'installed-ui.conf'
                path.write_text(self.PROFILE)
                path.chmod(0o600)
                before = path.stat()
                original_read = os.read
                replacement = self.PROFILE.replace('ultra-one', 'ultra-two')
                self.assertEqual(len(replacement), before.st_size)

                def mutate_after_read(fd, length):
                    raw = original_read(fd, length)
                    time.sleep(.02)  # Ensure a distinct real filesystem change date.
                    if atomic:
                        pending = path.with_suffix('.pending')
                        pending.write_text(replacement)
                        pending.chmod(0o600)
                        os.replace(pending, path)
                    else:
                        path.write_text(replacement)
                        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
                        self.assertNotEqual(path.stat().st_ctime_ns, before.st_ctime_ns)
                    return raw

                with patch('vehicle_fanout.os.read', mutate_after_read):
                    with self.assertRaisesRegex(ValueError, 'changed while reading'):
                        installed_ui_source(path, os.getuid())
                self.assertEqual(installed_ui_source(path, os.getuid())['vehicleSocket'],
                                 '/private/ultra-two.sock')


class ConfigurationTests(unittest.TestCase):
    def test_optional_hud_is_a_distinct_fourth_consumer(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            path = root / 'fanout.json'
            for names in (('backup', 'receiver', 'hud'), ('backup', 'receiver', 'ultra', 'hud')):
                consumers = {name: str(root / (name + '.sock')) for name in names}
                value = {'version': 1, 'input': str(root / 'adapter.sock'), 'consumers': consumers}
                path.write_text(json.dumps(value)); path.chmod(0o600)
                self.assertEqual(set(load_configuration(path)['consumers']), set(names))
                consumers['hud'] = consumers['receiver']
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError): load_configuration(path)

    def test_exact_distinct_private_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            path = root / 'fanout.json'
            value = {'version': 1, 'input': str(root/'adapter.sock'),
                     'consumers': {'backup': str(root/'backup.sock'), 'receiver': str(root/'receiver.sock')}}
            path.write_text(json.dumps(value)); path.chmod(0o600)
            self.assertEqual(load_configuration(path)['input'], root/'adapter.sock')
            for mutation in ({'version': True}, {'extra': 1},
                             {'input': str(root/'receiver.sock')},
                             {'input': str(root/'../adapter.sock')},
                             {'input': '/'+'x'*108},
                             {'consumers': {'backup': str(root/'b'), 'unknown': str(root/'r')}},
                             {'consumers': {'backup': str(root/'r'), 'receiver': str(root/'r')}}):
                path.write_text(json.dumps(value | mutation))
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    load_configuration(path)
            path.write_text(json.dumps(value)); path.chmod(0o644)
            with self.assertRaises(ValueError): load_configuration(path)
            path.chmod(0o600)
            with self.assertRaises(ValueError): load_configuration(path, os.getuid()+1)
            alias = root/'alias.json'; alias.symlink_to(path)
            with self.assertRaises(OSError): load_configuration(alias)
            private_parent(root/'adapter.sock', os.getuid())
            root.chmod(0o755)
            with self.assertRaises(ValueError): private_parent(root/'adapter.sock', os.getuid())
            root.chmod(0o700)

    def test_duplicate_configuration_field_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder).resolve()/'config.json'
            path.write_text('{"version":1,"version":1,"input":"/tmp/a","consumers":{}}')
            path.chmod(0o600)
            with self.assertRaises(ValueError): load_configuration(path)


class TimestampTests(unittest.TestCase):
    def ancillary(self, value):
        return [(socket.SOL_SOCKET, TIMESTAMPNS, TIMESPEC.pack(value//1_000_000_000, value%1_000_000_000))]

    def test_ingress_age_is_preserved_and_old_future_nested_packets_are_dropped(self):
        realtime, monotonic = 10_000_000_000, 7_000_000_000
        packet = b'HUV2 speed_kmh=42 low_fuel=on'
        result = ingress_packet(packet, 0, self.ancillary(realtime-20_000_000), realtime, monotonic)
        self.assertEqual(result, b'HUF1 6980000000 '+packet)
        for anc in ([], self.ancillary(realtime+1), self.ancillary(realtime-MAX_QUEUE_AGE_NS-1),
                    self.ancillary(realtime)*2, [(socket.SOL_SOCKET,TIMESTAMPNS,b'bad')]):
            with self.subTest(ancillary=anc):
                self.assertIsNone(ingress_packet(packet,0,anc,realtime,monotonic))
        anc = self.ancillary(realtime)
        for data in (b'', b'HUF1 7000000000 '+packet, b'x'*(MAX_PACKET+1), b'HUV2 speed_kmh=4\0'):
            self.assertIsNone(ingress_packet(data,0,anc,realtime,monotonic))
        for flag in (socket.MSG_TRUNC, socket.MSG_CTRUNC):
            self.assertIsNone(ingress_packet(packet,flag,anc,realtime,monotonic))

    def test_hvo2_transmission_bytes_are_forwarded_without_redating(self):
        realtime, monotonic = 10_000_000_000, 7_000_000_000
        packet = b'HVO2 1 2 6000000000 6900000000 transmission=automatic/drive/3,6800000000,500000000,m,a'
        ancillary = self.ancillary(realtime-20_000_000)
        self.assertEqual(ingress_packet(packet,0,ancillary,realtime,monotonic),packet)
        # Distribution is opaque; the C++ consumers validate complete snapshots.
        boundary = b'HVO2 '+b'x'*(MAX_PACKET-5)
        self.assertEqual(ingress_packet(boundary,0,ancillary,realtime,monotonic),boundary)
        for data in (boundary+b'x',b'HUF1 1 '+packet,b'HVO5 '+packet[5:],packet+b'\0'):
            self.assertIsNone(ingress_packet(data,0,ancillary,realtime,monotonic))
        for anc in ([],self.ancillary(realtime+1),self.ancillary(realtime-MAX_QUEUE_AGE_NS-1)):
            self.assertIsNone(ingress_packet(packet,0,anc,realtime,monotonic))
        for flag in (socket.MSG_TRUNC,socket.MSG_CTRUNC):
            self.assertIsNone(ingress_packet(packet,flag,ancillary,realtime,monotonic))

    def test_hvo3_catalog_and_observation_keep_original_bytes(self):
        realtime,monotonic=10_000_000_000,7_000_000_000
        packet=(b'HVO3 1 2 6000000000 6900000000 drive_modes=comfort,sport '
                b'drive_mode=sport,6800000000,500000000,m,a')
        ancillary=self.ancillary(realtime-20_000_000)
        self.assertEqual(ingress_packet(packet,0,ancillary,realtime,monotonic),packet)
        # Opaque distribution; C++ consumers require and validate all 36 records.
        boundary=b'HVO3 '+b'x'*(MAX_PACKET-5)
        self.assertEqual(ingress_packet(boundary,0,ancillary,realtime,monotonic),boundary)
        for data in (boundary+b'x',b'HUF1 1 '+packet,packet+b'\0'):
            self.assertIsNone(ingress_packet(data,0,ancillary,realtime,monotonic))
        for anc in ([],self.ancillary(realtime+1),self.ancillary(realtime-MAX_QUEUE_AGE_NS-1)):
            self.assertIsNone(ingress_packet(packet,0,anc,realtime,monotonic))
        for flag in (socket.MSG_TRUNC,socket.MSG_CTRUNC):
            self.assertIsNone(ingress_packet(packet,flag,ancillary,realtime,monotonic))

    def test_hvo4_trip_and_heading_bytes_keep_source_dates(self):
        realtime,monotonic=10_000_000_000,7_000_000_000
        packet=(b'HVO4 1 2 6000000000 6900000000 drive_modes=- '
                b'trip=-,6800000000,500000000,m,d heading=-,6700000000,500000000,m,d')
        ancillary=self.ancillary(realtime-20_000_000)
        self.assertEqual(ingress_packet(packet,0,ancillary,realtime,monotonic),packet)
        # The broker preserves bytes; consumer schema/history validation is separate.
        boundary=b'HVO4 '+b'x'*(MAX_PACKET-5)
        self.assertEqual(ingress_packet(boundary,0,ancillary,realtime,monotonic),boundary)
        for data in (boundary+b'x',b'HUF1 1 '+packet,packet+b'\0',b'HVO5 '+packet[5:]):
            self.assertIsNone(ingress_packet(data,0,ancillary,realtime,monotonic))
        for anc in ([],self.ancillary(realtime+1),self.ancillary(realtime-MAX_QUEUE_AGE_NS-1)):
            self.assertIsNone(ingress_packet(packet,0,anc,realtime,monotonic))
        for flag in (socket.MSG_TRUNC,socket.MSG_CTRUNC):
            self.assertIsNone(ingress_packet(packet,flag,ancillary,realtime,monotonic))

    def test_one_turn_cannot_drain_an_unbounded_adapter_queue(self):
        class QueuedSocket:
            remaining=65
            def recvmsg(self,*_):
                if not self.remaining: raise BlockingIOError()
                self.remaining-=1
                now=time.time_ns()
                ancillary=[(socket.SOL_SOCKET,TIMESTAMPNS,TIMESPEC.pack(now//1_000_000_000,now%1_000_000_000))]
                return b'HUV1 40 1200',ancillary,0,None
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve()
            broker=Fanout.__new__(Fanout)
            broker.owner=os.getuid(); broker.socket=QueuedSocket()
            broker.config={'consumers':{'backup':root/'absent.sock','receiver':root/'absent2.sock'}}
            broker.stats={'received':0,'discarded':0,'delivered':0,'unavailable':0}
            broker.turn()
            self.assertEqual(broker.socket.remaining,1)
            self.assertEqual(broker.stats['received'],64)
            broker.turn()
            self.assertEqual(broker.stats['received'],65)

    def test_observations_keep_original_bytes_and_legacy_limits(self):
        realtime, monotonic = 10_000_000_000, 7_000_000_000
        packet = b'HVO1 1 2 6000000000 6900000000 speed_kmh=42,6000000000,500000000,m,a'
        ancillary = self.ancillary(realtime-20_000_000)
        self.assertEqual(ingress_packet(packet,0,ancillary,realtime,monotonic),packet)
        # The broker is a bounded distributor, not a second schema validator.
        # Consumers reject incomplete/malformed HVO1 fields independently.
        boundary = b'HVO1 '+b'x'*(MAX_PACKET-5)
        self.assertEqual(ingress_packet(boundary,0,ancillary,realtime,monotonic),boundary)
        for data in (boundary+b'x',b'HUV2 '+b'x'*(MAX_LEGACY_PACKET-4),b'HUF1 1 '+packet):
            self.assertIsNone(ingress_packet(data,0,ancillary,realtime,monotonic))
        for anc in (self.ancillary(realtime+1),self.ancillary(realtime-MAX_QUEUE_AGE_NS-1)):
            self.assertIsNone(ingress_packet(packet,0,anc,realtime,monotonic))


@unittest.skipUnless(sys.platform.startswith('linux') and TIMESPEC.size==16, 'requires CM5/Linux kernel timestamp API')
class LinuxFanoutTests(unittest.TestCase):
    def fixture(self, root):
        return {'input': root/'adapter.sock', 'consumers': {'backup': root/'backup.sock', 'receiver': root/'receiver.sock'}}

    def consumer(self, path):
        value = socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
        value.bind(str(path)); os.chmod(path,0o600); value.settimeout(.05)
        self.addCleanup(value.close)
        return value

    def test_all_consumers_receive_identical_ingress_time_without_replay(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); config = self.fixture(root)
            receiver = self.consumer(config['consumers']['receiver'])
            with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                broker = Fanout(config)
                try:
                    source.sendto(b'HUV2 speed_kmh=42 low_fuel=on',str(config['input']))
                    broker.turn(); first = receiver.recv(4096)
                    self.assertTrue(first.startswith(b'HUF1 '))
                    self.assertEqual(first.split(b' ',2)[2],b'HUV2 speed_kmh=42 low_fuel=on')
                    self.assertEqual(broker.stats['unavailable'],1)
                    backup = self.consumer(config['consumers']['backup'])
                    broker.turn()
                    with self.assertRaises(socket.timeout): backup.recv(4096)
                    source.sendto(b'HUV1 0 0',str(config['input'])); broker.turn()
                    self.assertEqual(backup.recv(4096),receiver.recv(4096))
                    source.sendto(b'HUF1 1 HUV1 99 9000',str(config['input'])); broker.turn()
                    with self.assertRaises(socket.timeout): receiver.recv(4096)
                finally:
                    broker.close()
            self.assertFalse(config['input'].exists())

    def test_hvo2_uses_real_kernel_ingress_and_never_replays_to_new_consumer(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); config=self.fixture(root)
            receiver=self.consumer(config['consumers']['receiver'])
            broker=Fanout(config)
            try:
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                    # Complete V2 fixture: existing fields explicitly unknown,
                    # one coherent fresh transmission at its original date.
                    keys=('speed_kmh engine_rpm fuel_percent coolant_c oil_c oil_pressure_kpa battery_voltage '
                          'ambient_c range_km odometer_km left_turn right_turn hazard high_beam low_beam '
                          'position_lights front_fog rear_fog check_engine oil_pressure battery_charge '
                          'brake_fault parking_brake abs stability_control traction_control airbag seat_belt '
                          'door_open low_fuel coolant_warning washer_low tyre_pressure glow_plug').split()
                    publication=time.monotonic_ns()
                    fields=' '.join(key+'=-,-,500000000,m,u' for key in keys)
                    packet=(f'HVO2 1 1 {publication} {publication} {fields} '
                            f'transmission=automatic/drive/3,{publication},500000000,m,a').encode()
                    source.sendto(packet,str(config['input'])); broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),packet)
                    self.assertEqual(broker.stats['discarded'],0)
                    backup=self.consumer(config['consumers']['backup'])
                    broker.turn()
                    with self.assertRaises(socket.timeout): backup.recv(MAX_PACKET+1)
                    next_packet=packet.replace(b'HVO2 1 1 ',b'HVO2 1 2 ',1)
                    source.sendto(next_packet,str(config['input'])); broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),next_packet)
                    self.assertEqual(backup.recv(MAX_PACKET+1),next_packet)
            finally: broker.close()

    def test_hvo3_complete_catalog_is_distributed_without_ingress_redating_or_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve();config=self.fixture(root)
            receiver=self.consumer(config['consumers']['receiver']);broker=Fanout(config)
            try:
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                    keys=('speed_kmh engine_rpm fuel_percent coolant_c oil_c oil_pressure_kpa battery_voltage '
                          'ambient_c range_km odometer_km left_turn right_turn hazard high_beam low_beam '
                          'position_lights front_fog rear_fog check_engine oil_pressure battery_charge '
                          'brake_fault parking_brake abs stability_control traction_control airbag seat_belt '
                          'door_open low_fuel coolant_warning washer_low tyre_pressure glow_plug transmission').split()
                    publication=time.monotonic_ns()
                    fields=' '.join(key+'=-,-,500000000,m,u' for key in keys)
                    packet=(f'HVO3 1 1 {publication} {publication} drive_modes=comfort,sport {fields} '
                            f'drive_mode=sport,{publication},500000000,m,a').encode()
                    source.sendto(packet,str(config['input']));broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),packet)
                    backup=self.consumer(config['consumers']['backup']);broker.turn()
                    with self.assertRaises(socket.timeout):backup.recv(MAX_PACKET+1)
                    next_packet=packet.replace(b'HVO3 1 1 ',b'HVO3 1 2 ',1)
                    source.sendto(next_packet,str(config['input']));broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),next_packet)
                    self.assertEqual(backup.recv(MAX_PACKET+1),next_packet)
            finally:broker.close()

    def test_hvo4_complete_snapshot_never_replays_to_later_consumer(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve();config=self.fixture(root)
            receiver=self.consumer(config['consumers']['receiver']);broker=Fanout(config)
            try:
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                    keys=('speed_kmh engine_rpm fuel_percent coolant_c oil_c oil_pressure_kpa battery_voltage '
                          'ambient_c range_km odometer_km left_turn right_turn hazard high_beam low_beam '
                          'position_lights front_fog rear_fog check_engine oil_pressure battery_charge '
                          'brake_fault parking_brake abs stability_control traction_control airbag seat_belt '
                          'door_open low_fuel coolant_warning washer_low tyre_pressure glow_plug transmission drive_mode').split()
                    publication=time.monotonic_ns()
                    fields=' '.join(key+'=-,-,500000000,m,u' for key in keys)
                    packet=(f'HVO4 1 1 {publication} {publication} drive_modes=- {fields} '
                            f'trip=current/1/1/100/50/36/total/7.2,{publication},500000000,c,a '
                            f'heading=0/true_north/course_over_ground,{publication},500000000,m,a').encode()
                    source.sendto(packet,str(config['input']));broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),packet)
                    backup=self.consumer(config['consumers']['backup']);broker.turn()
                    with self.assertRaises(socket.timeout):backup.recv(MAX_PACKET+1)
                    next_packet=packet.replace(b'HVO4 1 1 ',b'HVO4 1 2 ',1)
                    source.sendto(next_packet,str(config['input']));broker.turn()
                    self.assertEqual(receiver.recv(MAX_PACKET+1),next_packet)
                    self.assertEqual(backup.recv(MAX_PACKET+1),next_packet)
            finally:broker.close()

    def test_queued_source_input_is_not_renewed_after_a_pause(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); config=self.fixture(root)
            receiver=self.consumer(config['consumers']['receiver'])
            broker=Fanout(config)
            try:
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                    source.sendto(b'HUV1 80 3000',str(config['input']))
                time.sleep(.3)
                broker.turn()
                self.assertEqual(broker.stats['discarded'],1)
                with self.assertRaises(socket.timeout): receiver.recv(4096)
            finally: broker.close()

    def test_real_daemon_kill_restart_and_term_preserve_new_packet_only(self):
        # Exercise the production CLI, kernel sockets and process ownership.
        # The broker is opaque: typed HVO validation remains a consumer check.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); config = self.fixture(root)
            profile = root / 'daemon.json'
            profile.write_text(json.dumps({'version': 1, 'input': str(config['input']),
                'consumers': {name: str(path) for name, path in config['consumers'].items()}}))
            profile.chmod(0o600)
            receiver = self.consumer(config['consumers']['receiver'])
            backup = self.consumer(config['consumers']['backup'])
            script = Path(__file__).resolve().parents[1] / 'tools/vehicle_fanout.py'
            child = None

            def start():
                nonlocal child
                child = subprocess.Popen([sys.executable, '-B', str(script), '--config', str(profile)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                limit = time.monotonic() + 3
                # A socket pathname can survive SIGKILL. Readiness therefore
                # needs delivery by this process, not only a retained inode.
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as source:
                    while time.monotonic() < limit:
                        self.assertIsNone(child.poll(), 'broker stopped during startup')
                        try:
                            source.sendto(b'HUV1 7 700', str(config['input']))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            time.sleep(.01)
                    else:
                        self.fail('broker did not accept its startup packet')
                # Never resubmit a successfully queued probe: that could leave
                # duplicate startup packets and confuse the no-replay check.
                for consumer in (receiver, backup):
                    consumer.settimeout(.5)
                try:
                    first, second = receiver.recv(MAX_PACKET + 1), backup.recv(MAX_PACKET + 1)
                    self.assertEqual(first, second)
                    self.assertEqual(first.split(b' ', 2)[2], b'HUV1 7 700')
                finally:
                    for consumer in (receiver, backup): consumer.settimeout(.05)

            try:
                start()
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as source:
                    source.sendto(b'HVO4 1 2 6000000000 6900000000 drive_modes=-', str(config['input']))
                    expected = b'HVO4 1 2 6000000000 6900000000 drive_modes=-'
                    self.assertEqual(receiver.recv(MAX_PACKET + 1), expected)
                    self.assertEqual(backup.recv(MAX_PACKET + 1), expected)
                child.kill(); child.communicate(timeout=3)
                self.assertTrue(config['input'].exists(), 'killed owner fixture should leave its socket')
                start()  # Reclaims only the dead endpoint after its flock exits.
                with self.assertRaises(socket.timeout): receiver.recv(MAX_PACKET + 1)
                with self.assertRaises(socket.timeout): backup.recv(MAX_PACKET + 1)
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as source:
                    source.sendto(b'HUV1 42 1200', str(config['input']))
                    first, second = receiver.recv(MAX_PACKET + 1), backup.recv(MAX_PACKET + 1)
                    self.assertEqual(first, second)
                    self.assertEqual(first.split(b' ', 2)[2], b'HUV1 42 1200')
                child.terminate(); _, error = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, error.decode(errors='replace'))
                self.assertFalse(config['input'].exists(), 'graceful owner must retire its endpoint')
            finally:
                if child is not None and child.poll() is None:
                    child.kill(); child.communicate(timeout=3)

    def test_unsafe_consumer_and_duplicate_or_live_input_owner_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); config=self.fixture(root)
            live=self.consumer(config['input'])
            with self.assertRaises(ValueError): Fanout(config)
            live.close(); config['input'].unlink()
            broker=Fanout(config)
            try:
                with self.assertRaises(OSError): Fanout(config)
                receiver=self.consumer(config['consumers']['receiver'])
                os.chmod(config['consumers']['receiver'],0o666)
                config['consumers']['backup'].symlink_to(config['consumers']['receiver'])
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as source:
                    source.sendto(b'HUV1 42 1000',str(config['input']))
                broker.turn()
                self.assertEqual(broker.stats['unavailable'],2)
                with self.assertRaises(socket.timeout): receiver.recv(4096)
            finally: broker.close()

    def test_stale_input_can_be_reclaimed_but_cleanup_does_not_unlink_a_replacement(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); config=self.fixture(root)
            dead=self.consumer(config['input']); dead.close()
            broker=Fanout(config)
            moved=root/'old.sock'; config['input'].rename(moved)
            replacement=self.consumer(config['input'])
            broker.close()
            self.assertTrue(config['input'].exists())


if __name__=='__main__': unittest.main()
