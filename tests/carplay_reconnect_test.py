# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import contextlib
import io
import signal
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from carplay_reconnect import ReconnectPolicy, has_control_connection
import carplay_reconnect
from request_carplay_connection import endpoint, request_connection
from receiver_configuration import validate_device_settings

PHONE = "02:11:22:33:44:55"
TARGET = ("10.10.0.30", 1234)


class ReconnectTest(unittest.TestCase):
    def run_helper(self, discover, active=None, request=None, paired=None, seconds=5,
                   diagnostics=False, output=None, diagnostic_clock=None):
        # Execute the real loop/policy with explicit simulated discovery cost,
        # presence and a monotonic clock. No Bluetooth/AP/phone is exercised.
        clock = [0.]
        handlers = {}
        requests = []
        def sleep(delay): clock[0] += delay
        def connect(*args):
            requests.append(clock[0])
            return request(*args) if request else 200
        with tempfile.TemporaryDirectory() as folder:
            identity = Path(folder) / 'identity.conf'
            identity.write_text('deviceid=02:11:22:33:44:55\nsrcvers=950.7.1\n')
            arguments = ['carplay_reconnect.py', '--phone-address', PHONE, '--identity', str(identity),
                         '--seconds', str(seconds), '--max-attempts', '1']
            with patch.object(sys, 'argv', arguments), \
                    patch.dict(carplay_reconnect.os.environ, {'HEADUNIT_CONNECTION_DIAGNOSTICS': '1' if diagnostics else '0'}), \
                    patch.object(carplay_reconnect.time, 'monotonic', side_effect=lambda: clock[0]), \
                    patch.object(carplay_reconnect.time, 'sleep', side_effect=sleep), \
                    patch.object(carplay_reconnect.time, 'clock_gettime_ns', side_effect=diagnostic_clock,
                                 wraps=carplay_reconnect.time.clock_gettime_ns if diagnostic_clock is None else None), \
                    patch.object(carplay_reconnect.signal, 'signal', side_effect=lambda number, handler: handlers.update({number: handler})), \
                    patch.object(carplay_reconnect, 'paired_phone', side_effect=lambda _: paired(clock) if paired else True), \
                    patch.object(carplay_reconnect, 'discovered_phone', side_effect=lambda _: discover(clock, handlers)), \
                    patch.object(carplay_reconnect, 'active_phone', side_effect=lambda: active(clock) if active else bool(requests)), \
                    patch.object(carplay_reconnect, 'receiver_listening', return_value=True), \
                    patch.object(carplay_reconnect, 'request_connection', side_effect=connect), \
                    contextlib.redirect_stdout(output if output is not None else io.StringIO()):
                carplay_reconnect.main()
        return requests, clock[0]

    def test_phone_service_after_initial_scan_does_not_wait_full_postscan_pause(self):
        scans = []
        def discover(clock, _):
            scans.append(clock[0])
            clock[0] += 1.  # Explicit one-second discovery boundary.
            return TARGET if clock[0] >= 1.01 else None
        requests, _ = self.run_helper(discover)
        self.assertEqual(len(requests), 1)
        # Service appears just after the first scan: next scan and request
        # finish by 2.5s, rather than waiting two seconds before scanning again.
        self.assertLessEqual(requests[0], 2.5)
        self.assertEqual(len(scans), 2)

    def test_control_established_during_discovery_prevents_request(self):
        def discover(clock, _): clock[0] += 1.; return TARGET
        requests, _ = self.run_helper(discover, active=lambda clock: clock[0] >= 1.)
        self.assertEqual(requests, [])

    def test_failed_inspection_recovers_without_rearming_failed_request_budget(self):
        inspections = []
        def paired(clock):
            inspections.append(clock[0])
            if len(inspections) == 1: raise OSError('Explicit unavailable inspection fixture')
            return True
        def request(*_): raise ConnectionError('Explicit failed request fixture')
        requests, _ = self.run_helper(lambda *_: TARGET, active=lambda _: False, request=request, paired=paired)
        self.assertEqual(len(requests), 1)
        self.assertGreaterEqual(requests[0], 0.5)
        self.assertGreater(len(inspections), 2)

    def test_fast_inspection_failure_and_active_session_poll_do_not_spin(self):
        inspections = []
        def unavailable(clock):
            inspections.append(clock[0])
            raise OSError('Explicit unavailable inspection fixture')
        def forbidden(*_): raise AssertionError('Discovery must not run')
        requests, _ = self.run_helper(forbidden, paired=unavailable, seconds=2)
        self.assertEqual(requests, [])
        self.assertLessEqual(len(inspections), 5)
        self.assertGreaterEqual(len(inspections), 3)
        polls = []
        def active(clock): polls.append(clock[0]); return True
        requests, _ = self.run_helper(forbidden, active=active, seconds=5)
        self.assertEqual(requests, [])
        self.assertEqual(len(polls), 3)
        self.assertGreaterEqual(polls[-1], 4.)

    def test_stop_or_lifetime_expiry_during_discovery_cannot_request(self):
        for stop in (False, True):
            def discover(clock, handlers):
                if stop: handlers[signal.SIGTERM](signal.SIGTERM, None)
                else: clock[0] += 6.
                return TARGET
            with self.subTest(stop=stop):
                requests, _ = self.run_helper(discover)
                self.assertEqual(requests, [])

    def test_connection_trace_is_default_off_bounded_and_content_free(self):
        output = io.StringIO()
        requests, _ = self.run_helper(lambda *_: TARGET, output=output)
        self.assertEqual(len(requests), 1)
        self.assertNotIn('CarPlay connection diagnostic', output.getvalue())
        output = io.StringIO()
        requests, _ = self.run_helper(lambda *_: TARGET, active=lambda _: False, seconds=20,
                                      diagnostics=True, output=output)
        self.assertEqual(len(requests), 1)
        rows = [line for line in output.getvalue().splitlines() if line.startswith('CarPlay connection diagnostic ')]
        self.assertLessEqual(len(rows), 64)
        self.assertLess(len(rows), 20)
        for stage in ('discovery-begin', 'discovery-end', 'request-begin', 'request-end'):
            self.assertTrue(any('stage=' + stage in row for row in rows))
        allowed = {'mono_ns', 'stage', 'attempt', 'paired', 'targetPresent', 'active', 'httpStatus', 'error'}
        dates = []
        for row in rows:
            fields = dict(field.split('=', 1) for field in row.split()[3:])
            self.assertLessEqual(set(fields), allowed)
            dates.append(int(fields['mono_ns']))
        self.assertTrue(all(date > 0 for date in dates))
        self.assertEqual(dates, sorted(dates))
        for private in (PHONE, TARGET[0], '950.7.1', 'deviceid', 'srcvers'):
            self.assertNotIn(private, '\n'.join(rows))

    def test_connection_trace_keeps_budget_for_late_phone_discovery(self):
        output = io.StringIO()
        scans = [0]
        def discover(clock, _):
            scans[0] += 1
            clock[0] += 0.25
            return TARGET if clock[0] >= 40 else None
        requests, _ = self.run_helper(discover, active=lambda _: False, seconds=45,
                                      diagnostics=True, output=output)
        self.assertEqual(len(requests), 1)
        self.assertGreater(scans[0], 64)
        rows = [line for line in output.getvalue().splitlines() if line.startswith('CarPlay connection diagnostic ')]
        self.assertLess(len(rows), 20)
        self.assertTrue(any('stage=request-end' in row for row in rows))
        found = next(index for index, row in enumerate(rows) if 'stage=discovery-end' in row and 'targetPresent=1' in row)
        self.assertIn('stage=discovery-begin', rows[found - 1])

    def test_optional_connection_trace_failure_does_not_change_request(self):
        output = io.StringIO()
        def unavailable_clock(*_): raise OSError('Explicit diagnostic clock failure')
        requests, _ = self.run_helper(lambda *_: TARGET, diagnostics=True, output=output,
                                      diagnostic_clock=unavailable_clock)
        self.assertEqual(len(requests), 1)
        self.assertIn('mono_ns=none stage=request-begin', output.getvalue())
        class UnwritableDiagnostic(io.StringIO):
            def write(self, text):
                if text.startswith('CarPlay connection diagnostic'): raise OSError('Explicit diagnostic output failure')
                return super().write(text)
        requests, _ = self.run_helper(lambda *_: TARGET, diagnostics=True, output=UnwritableDiagnostic())
        self.assertEqual(len(requests), 1)

    def test_request_budget_and_active_session_recovery(self):
        policy = ReconnectPolicy()
        self.assertFalse(policy.ready(0, None, False))
        self.assertTrue(policy.ready(1, TARGET, False))
        policy.attempted(1)
        self.assertFalse(policy.ready(15, TARGET, False))
        self.assertTrue(policy.ready(16, TARGET, False))
        policy.attempted(16)
        self.assertTrue(policy.ready(31, TARGET, False))
        policy.attempted(31)
        self.assertFalse(policy.ready(1000, TARGET, False))
        # A session starts: discovery repeats must never interrupt it.
        for at in (1001, 1010, 1100):
            self.assertFalse(policy.ready(at, TARGET, True))
        self.assertFalse(policy.ready(1101, TARGET, False))
        self.assertTrue(policy.ready(1102, TARGET, False))
        self.assertEqual(policy.attempts, 0)

    def test_presence_change_rearms_exhausted_budget(self):
        policy = ReconnectPolicy(max_attempts=1)
        self.assertTrue(policy.ready(0, TARGET, False))
        policy.attempted(0)
        self.assertFalse(policy.ready(100, TARGET, False))
        self.assertFalse(policy.ready(101, None, False))
        self.assertTrue(policy.ready(102, TARGET, False))

    def test_discovery_selects_phone_and_ap_only(self):
        line = '=;wlan0;IPv4;phone;_carplay-ctrl._tcp;local;phone.local;10.10.0.30;1234;"id=02:11:22:33:44:55"'
        self.assertEqual(endpoint(line, PHONE), TARGET)
        for invalid in (line.replace('wlan0', 'eth0'), line.replace(PHONE, '02:00:00:00:00:01'),
                        line.replace('10.10.0.30', '192.168.1.10'), line.replace('10.10.0.30', '10.10.0.1'),
                        line.replace(';1234;', ';0;'), line.replace(';1234;', ';65536;')):
            self.assertIsNone(endpoint(invalid, PHONE))
        with self.assertRaises(ValueError):
            validate_device_settings({'auto_connect_phone': 'any'})
        validate_device_settings({'auto_connect_phone': PHONE})

    def test_retry_configuration_bounds(self):
        validate_device_settings({'auto_connect_retry_seconds': 20, 'auto_connect_max_attempts': 2})
        for key, value in (('auto_connect_retry_seconds', 0), ('auto_connect_retry_seconds', 601),
                           ('auto_connect_max_attempts', 0), ('auto_connect_max_attempts', 11),
                           ('auto_connect_max_attempts', True)):
            with self.assertRaises(ValueError):
                validate_device_settings({key: value})

    def test_active_ipv4_and_mapped_ipv6(self):
        for output in ('0 0 10.10.0.1:7000 10.10.0.30:50000',
                       '0 0 [::ffff:10.10.0.1]:7000 [::ffff:10.10.0.30]:50000'):
            self.assertTrue(has_control_connection(output))
            self.assertTrue(has_control_connection(output, '10.10.0.30'))
            self.assertFalse(has_control_connection(output, '10.10.0.40'))
        self.assertFalse(has_control_connection('', '10.10.0.30'))
        with self.assertRaises(ValueError):
            has_control_connection('unexpected output', '10.10.0.30')

    def test_http_request_and_rejection(self):
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                received.append((self.path, self.headers['AirPlay-Receiver-Device-ID'], self.headers['User-Agent']))
                self.send_response(200 if len(received) == 1 else 503)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def log_message(self, *args):
                pass

        with patch('socket.getfqdn', return_value='localhost'):
            server = HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            self.assertEqual(request_connection(server.server_address, PHONE, '950.7.1'), 200)
            self.assertEqual(request_connection(server.server_address, PHONE, '950.7.1'), 503)
            self.assertEqual(received[0], ('/ctrl-int/1/connect', str(int('021122334455', 16)), 'AirPlay/950.7.1'))
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
