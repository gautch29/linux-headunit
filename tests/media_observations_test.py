# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Production manager fixtures; substituted BlueZ/child, no phone or audio device."""
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_manager import Manager
from media_state import MediaObservations
from audio_control import AudioControl
from phone_state import atomic_json

PHONE = 'AA:BB:CC:DD:EE:01'


class Phones:
    def __init__(self):
        self.commands = []

    def phones(self):
        return [{'address': PHONE}]

    def phone(self, phone):
        return {'address': phone}

    def media_command(self, phone, command):
        self.commands.append(command)

    def media(self, phone):
        return {'available': True, 'source': 'bluetooth', 'status': 'playing',
                'controls': ['play']}


class Child:
    returncode = None

    def poll(self):
        return self.returncode

    def terminate(self):
        self.returncode = 0

    def wait(self, timeout):
        return self.returncode


class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name).resolve()
        display = root / 'standard.conf'
        display.write_text('explicit unused display fixture')
        config = root / 'receiver.json'
        config.write_text(json.dumps({'pair_store': str(root / 'pairs'), 'display_config': str(display)}))
        self.manager = Manager(config, root, display, (os.getuid(), os.getgid()), bluez=Phones())
        self.addCleanup(self.manager.shutdown)
        self.manager.state.select(PHONE)
        self.manager.active = PHONE
        self.manager.child = Child()
        self.manager.media.publish({'available': True, 'source': 'bluetooth',
                                    'status': 'paused', 'controls': ['play']})

    def play(self):
        return self.manager.request({'action': 'media', 'phone': PHONE,
                                     'generation': self.manager.generation, 'command': 'play'})

    def test_normal_receiver_exit_rejects_media_before_polling_retirement(self):
        # Exact interval between child exit and the manager's next tick. A
        # retained active phone/generation and capabilities are insufficient.
        self.manager.child.returncode = 7
        with self.assertRaisesRegex(ValueError, 'receiver'):
            self.play()
        self.assertEqual(self.manager.bluez.commands, [])

    def status(self):
        return self.manager.request({'action': 'status'})

    def test_exited_receiver_clears_status_without_waiting_for_tick(self):
        self.play()
        self.manager.child.returncode = 0
        status = self.status()
        self.assertFalse(status['media']['available'])
        self.assertEqual(status['media']['controls'], [])
        observations = status['media_observations']
        self.assertFalse(observations['live'])
        self.assertFalse(observations['transportReport']['available'])
        self.assertIsNone(observations['action'])

    def test_receiver_exit_during_bluez_lookup_cannot_use_earlier_liveness(self):
        def lookup(phone):
            self.manager.child.returncode = 0
            return {'address': phone}
        self.manager.bluez.phone = lookup
        with self.assertRaisesRegex(ValueError, 'receiver'):
            self.play()
        self.assertEqual(self.manager.bluez.commands, [])

    def test_slow_status_lookup_cannot_refresh_expired_transport(self):
        with patch('phone_manager.time.monotonic', return_value=50) as clock:
            self.manager.media.publish({'available': True, 'source': 'bluetooth',
                                        'status': 'paused', 'controls': ['play']})
            def phones():
                clock.return_value = 54
                return []
            self.manager.bluez.phones = phones
            status = self.status()
        self.assertFalse(status['media']['available'])
        self.assertFalse(status['media_observations']['transportReport']['fresh'])

    def test_stale_future_and_missing_publication_do_not_authorize_bluetooth(self):
        for date in (46000, 51000, None):
            with self.subTest(date=date), patch('phone_manager.time.monotonic', return_value=50):
                self.manager.media.last['updated_monotonic_ms'] = date
                with self.assertRaisesRegex(ValueError, 'unavailable'):
                    self.play()
                status = self.status()
                self.assertFalse(status['media']['available'])
                report = status['media_observations']['transportReport']
                self.assertFalse(report['available'])
                self.assertEqual(report['reportedState'], 'paused')
                self.assertEqual(report['publishedMonotonicMs'], date)
                self.assertIsNone(report['reportAcquiredMonotonicNs'])
        self.assertEqual(self.manager.bluez.commands, [])

    def test_dispatch_and_pending_are_not_playback_confirmation(self):
        captured = []
        def dispatch(phone, command):
            captured.append(self.status()['media_observations']['action'])
        self.manager.bluez.media_command = dispatch
        reply = self.play()
        self.assertFalse(reply['confirmed'])
        self.assertEqual(reply['completion'], 'unknown')
        self.assertEqual(captured[0]['state'], 'pending')
        self.assertFalse(captured[0]['confirmed'])
        status = self.status()
        self.assertEqual(status['media']['status'], 'paused')
        action = status['media_observations']['action']
        self.assertEqual(action['state'], 'dispatched')
        self.assertEqual(action['completion'], 'unknown')
        self.assertGreaterEqual(int(action['resultMonotonicNs']), int(action['requestedMonotonicNs']))
        self.assertFalse(status['media_observations']['playbackConfirmed'])
        self.manager.media.publish({'available': True, 'source': 'bluetooth',
                                    'status': 'playing', 'controls': ['play']})
        self.assertEqual(self.status()['media_observations']['action']['completion'], 'unknown')

    def test_dispatch_failure_is_bounded_and_does_not_invent_applied_state(self):
        self.manager.bluez.media_command = lambda *args: (_ for _ in ()).throw(ValueError('private fixture details'))
        with self.assertRaisesRegex(ValueError, 'private fixture'):
            self.play()
        action = self.status()['media_observations']['action']
        self.assertEqual(action['error'], 'dispatch-failed')
        self.assertEqual(action['state'], 'error')
        self.assertFalse(action['confirmed'])

    def test_stop_retires_pending_action_and_late_result_cannot_replace_successor(self):
        old = self.manager.generation
        captured = []
        def dispatch(phone, command):
            captured.append(self.status()['media_observations']['action']['id'])
            self.manager.stop()
            self.manager.active = PHONE
            self.manager.child = Child()
            self.manager.media.publish({'available': True, 'source': 'bluetooth',
                                        'status': 'paused', 'controls': ['play']})
            self.manager.bluez.media_command = lambda *args: None
            self.play()
            captured.append(self.status()['media_observations']['action']['id'])
        self.manager.bluez.media_command = dispatch
        self.play()
        action = self.status()['media_observations']['action']
        self.assertEqual(action['id'], captured[1])
        self.assertNotEqual(action['id'], captured[0])
        self.assertGreater(action['generation'], old)
        with self.assertRaisesRegex(ValueError, 'Session changed'):
            self.manager.request({'action': 'media', 'phone': PHONE, 'generation': old, 'command': 'play'})

    def test_media_rejects_boolean_generation_and_incomplete_or_extra_fields(self):
        for fields in ({'generation': False}, {'extra': True}, {'command': None},
                       {'command': []}, {'command': {'play': True}}):
            request = {'action': 'media', 'phone': PHONE, 'generation': 0, 'command': 'play'}
            request.update(fields)
            with self.assertRaises(ValueError):
                self.manager.request(request)
        self.assertEqual(self.manager.bluez.commands, [])

    def test_hid_is_a_queued_toggle_even_without_current_transport_report(self):
        self.manager.active_mode = 'ultra'
        self.manager.layout_path = Path(self.temporary.name) / 'unused-layout-fixture'
        self.manager.carplay.last = {'connectionGeneration': '1'}
        # IPC substitute only: manager admission and the published state are
        # production. This does not establish phone HID acceptance.
        with patch.object(self.manager, 'carplay_snapshot', return_value={'returnReady': True}), \
             patch('phone_manager.layout_navigate') as navigate:
            self.manager.media.last['updated_monotonic_ms'] = -10000
            reply = self.play()
            self.assertEqual(navigate.call_args.args[1], 'play_pause')
        self.assertEqual(reply['transport'], 'ultra-hid')
        self.assertFalse(reply['confirmed'])
        action = self.manager.media_observations.action
        self.assertEqual(action['state'], 'queued')
        self.assertEqual(action['completion'], 'unknown')
        self.assertEqual(self.manager.bluez.commands, [])

    def test_poll_cannot_republish_media_after_receiver_exits_during_lookup(self):
        # Native cluster reads media.json directly, independently of status's
        # liveness filtering. Exercise both production publication routes.
        for source in ('iap', 'bluetooth'):
            for failure in (0, -9):
                with self.subTest(source=source, exit=failure):
                    child = self.manager.child = subprocess.Popen(
                        [sys.executable, '-B', '-c', 'import sys\nsys.stdin.buffer.read(1)\n'],
                        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

                    def reap(child=child):
                        if child.poll() is None:
                            child.kill()
                        child.wait(timeout=3)
                        child.stdin.close()
                    self.addCleanup(reap)
                    if source == 'iap':
                        atomic_json(self.manager.iap_media_path,
                                    {'available': True, 'title': 'Retiring fixture',
                                     'status': 'paused'}, self.manager.owner)
                    else:
                        self.manager.iap_media_path.unlink(missing_ok=True)
                    self.manager.retirement_required = False

                    def lookup(phone):
                        if failure < 0:
                            child.kill()
                        else:
                            child.stdin.close()
                        self.assertEqual(child.wait(timeout=3), failure)
                        return {'available': True, 'source': 'bluetooth',
                                'title': 'Retiring fixture', 'status': 'playing',
                                'controls': ['play']}
                    self.manager.bluez.media = lookup
                    self.manager.tick()
                    published = json.loads(self.manager.media.path.read_bytes())
                    self.assertFalse(published['available'])
                    self.assertEqual(published['controls'], [])
                    self.assertNotIn('title', published)
                    self.assertNotIn('artwork', published)
                    self.assertEqual(self.manager.retirement_required, failure < 0)
                    # Preserve the existing normal-exit and whole-service
                    # signal-retirement paths, then admit a fresh test owner.
                    if failure < 0:
                        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                            self.manager.tick()
                        self.manager.retirement_required = False
                    else:
                        self.manager.tick()
                    self.manager.active = PHONE
                    self.manager.child = Child()
                    self.manager.bluez.media = Phones().media
                    self.manager.tick()
                    self.assertTrue(self.manager.media.last['available'])

    def test_poll_exception_after_receiver_exit_cannot_republish_fallback(self):
        atomic_json(self.manager.iap_media_path,
                    {'available': True, 'title': 'Retiring fixture'}, self.manager.owner)
        def lookup(phone):
            self.manager.child.returncode = 0
            raise ValueError('Explicit unavailable BlueZ fixture')
        self.manager.bluez.media = lookup
        self.manager.tick()
        self.assertFalse(json.loads(self.manager.media.path.read_bytes())['available'])

    def test_poll_late_result_cannot_replace_successor_content(self):
        for source in ('iap', 'bluetooth'):
            with self.subTest(source=source):
                if source == 'iap':
                    atomic_json(self.manager.iap_media_path,
                                {'available': True, 'title': 'Retiring fixture'}, self.manager.owner)
                else:
                    self.manager.iap_media_path.unlink(missing_ok=True)
                def lookup(phone):
                    self.manager.stop()
                    self.manager.active = PHONE
                    self.manager.child = Child()
                    self.manager.media.publish({'available': True, 'source': 'bluetooth',
                                                'title': 'Successor fixture', 'status': 'paused'})
                    self.manager.bluez.media = Phones().media
                    return {'available': True, 'source': 'bluetooth', 'title': 'Late fixture'}
                self.manager.bluez.media = lookup
                self.manager.tick()
                published = json.loads(self.manager.media.path.read_bytes())
                self.assertEqual(published['title'], 'Successor fixture')
                self.assertEqual(published['status'], 'paused')

    def test_iap_paused_and_real_file_bridge_pcm_progress_remain_separate(self):
        self.manager.iap_media_path.write_text(json.dumps({'available': True, 'status': 'paused'}))
        self.manager.tick()
        self.manager.audio = AudioControl(Path(self.temporary.name), (os.getuid(), os.getgid()), clock=lambda: self.now)
        self.now = 10_000_000_000
        state = {'version': 1, 'updatedMonotonicNs': str(self.now), 'available': True,
                 'route': 'a' * 32, 'acceptedSequence': '0', 'appliedSequence': '0',
                 'requestedVolumePercent': 100, 'requestedMuted': False,
                 'volumePercent': 100, 'muted': False, 'error': '',
                 'output': {'sampledMonotonicNs': str(self.now), 'acceptedBlocks': '100',
                            'discontinuities': '0', 'underruns': '0', 'starved': False}}
        def publish():
            atomic_json(self.manager.audio.state_path, state)
            with patch('phone_manager.time.clock_gettime_ns', return_value=self.now):
                return self.status()
        with patch.object(self.manager.audio, 'snapshot', wraps=self.manager.audio.snapshot) as snapshot:
            first = publish()
            self.assertEqual(snapshot.call_count, 1)
        self.assertEqual(first['media_observations']['pcm']['state'], 'baseline')
        self.now += 100_000_000
        state['updatedMonotonicNs'] = str(self.now)
        state['output'].update(sampledMonotonicNs=str(self.now), acceptedBlocks='110')
        second = publish()
        self.assertEqual(second['media']['status'], 'paused')
        self.assertEqual(second['media']['source'], 'iap')
        pcm = second['media_observations']['pcm']
        self.assertEqual(pcm['state'], 'progressing')
        self.assertEqual(pcm['acceptedBlocksDelta'], '10')
        self.assertEqual(pcm['previousSampledMonotonicNs'], '10000000000')
        self.assertEqual(pcm['sampledMonotonicNs'], '10100000000')
        self.assertFalse(second['media_observations']['playbackConfirmed'])
        self.assertEqual(publish()['media_observations']['pcm']['state'], 'unchanged-sample')
        # A fresh publication must not renew the original output observation.
        self.now += 1_000_000_001
        state['updatedMonotonicNs'] = str(self.now)
        stale = publish()
        self.assertEqual(stale['media_observations']['pcm']['state'], 'stale')
        self.manager.stop()
        self.assertEqual(self.status()['media_observations']['pcm']['state'], 'unavailable')


class ProjectionTests(unittest.TestCase):
    def setUp(self):
        self.model = MediaObservations()
        self.sample = 10_000_000_000
        self.generation = 1
        self.audio = {'available': True, 'route': 'a' * 32, 'output': {
            'sampledMonotonicNs': str(self.sample), 'acceptedBlocks': '100',
            'discontinuities': '0', 'underruns': '0', 'starved': False, 'fresh': True}}

    def row(self, live=True):
        return self.model.snapshot(self.generation, live, {}, self.audio, 50, self.sample)['pcm']

    def advance(self, count='110'):
        self.sample += 100_000_000
        self.audio['output'].update(sampledMonotonicNs=str(self.sample), acceptedBlocks=count)

    def test_same_date_mutation_regression_and_recovery_preserve_frontier(self):
        self.row()
        self.audio['output']['acceptedBlocks'] = '110'
        self.assertEqual(self.row()['state'], 'invalid-observation')
        self.audio['output']['acceptedBlocks'] = '99'
        self.advance('99')
        self.assertEqual(self.row()['state'], 'invalid-observation')
        self.advance('120')
        row = self.row()
        self.assertEqual(row['acceptedBlocksDelta'], '20')
        self.assertEqual(row['previousSampledMonotonicNs'], '10000000000')
        self.audio['output']['sampledMonotonicNs'] = '9999999999'
        self.assertEqual(self.row()['state'], 'invalid-observation')

    def test_generation_route_loss_and_legacy_output_start_new_baselines(self):
        self.row()
        for boundary in ('generation', 'route', 'unavailable', 'legacy', 'dead'):
            with self.subTest(boundary=boundary):
                if boundary == 'generation':
                    self.generation += 1
                elif boundary == 'route':
                    self.audio['route'] = 'b' * 32
                elif boundary == 'dead':
                    self.row(live=False)
                elif boundary == 'unavailable':
                    self.audio['available'] = False
                    self.assertEqual(self.row()['state'], 'unavailable')
                    self.audio['available'] = True
                else:
                    output = self.audio.pop('output')
                    self.assertEqual(self.row()['state'], 'unavailable')
                    self.audio['output'] = output
                self.advance()
                row = self.row()
                self.assertEqual(row['state'], 'baseline')
                self.assertIsNone(row['acceptedBlocksDelta'])

    def test_repeated_samples_no_advance_and_old_intervals_do_not_count_progress(self):
        self.row()
        self.assertEqual(self.row()['state'], 'unchanged-sample')
        self.advance('100')
        self.assertEqual(self.row()['state'], 'no-advance')
        self.sample += 2_000_000_000
        self.audio['output'].update(sampledMonotonicNs=str(self.sample), acceptedBlocks='200')
        self.assertEqual(self.row()['state'], 'baseline')

    def test_same_date_timing_and_counters_are_not_mutable_by_consumer(self):
        self.audio['output']['timing'] = {'scheduledAheadNs': '100'}
        self.row()
        self.audio['output']['timing']['scheduledAheadNs'] = '99'
        self.assertEqual(self.row()['state'], 'invalid-observation')
        self.audio['output']['timing']['scheduledAheadNs'] = '100'
        self.audio['output']['starved'] = True
        self.assertEqual(self.row()['state'], 'invalid-observation')

    def test_route_change_without_telemetry_and_old_action_result_are_retired(self):
        self.row()
        output = self.audio.pop('output')
        self.audio['route'] = 'b' * 32
        self.row()
        self.audio['output'] = output
        self.advance()
        self.assertEqual(self.row()['state'], 'baseline')
        token = self.model.begin(1, 'play', 'bluetooth', 1)
        self.model.reset()
        successor = self.model.begin(2, 'pause', 'bluetooth', 2)
        self.model.finish(token, 'error', 3)
        self.assertEqual(self.model.action['id'], successor)
        self.assertEqual(self.model.action['state'], 'pending')


if __name__ == '__main__':
    unittest.main()
