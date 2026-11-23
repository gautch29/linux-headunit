# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Non-visual sport engine. All clocks are original host-monotonic acquisitions.

No GPIO, actuator, phone, GTK or signed-scene dependency. Pure model updates
are bounded; the manager adapter owns acquisition, polling and history I/O.
"""
from dataclasses import dataclass
from collections import deque
import copy
import math

NS = 1_000_000_000
G = 9.80665
EARTH = 6_371_000.0
ORIGINS = ('measured', 'calculated', 'simulation')


@dataclass(frozen=True)
class Observation:
    value: object
    acquired_ns: int | None
    maximum_age_ns: int = 500_000_000
    origin: str = 'measured'
    availability: str = 'available'

    def valid(self, now):
        return (type(now) is int and now > 0 and
                type(self.maximum_age_ns) is int and 0 < self.maximum_age_ns <= 86400 * NS and
                self.origin in ORIGINS and self.availability in
                ('available', 'unavailable', 'stale', 'disconnected') and
                (self.acquired_ns is None or type(self.acquired_ns) is int and
                 0 < self.acquired_ns <= now) and
                (self.value is not None) == (self.availability == 'available') and
                (self.availability not in ('available', 'stale') or self.acquired_ns is not None))

    def fresh(self, now):
        return (self.valid(now) and self.availability == 'available' and
                now - self.acquired_ns < self.maximum_age_ns)

    def model(self, now):
        state = self.availability
        if state == 'available' and not self.fresh(now):
            state = 'stale'
        return {'value': self.value if state == 'available' else None,
                'acquiredNs': self.acquired_ns, 'maximumAgeNs': self.maximum_age_ns,
                'ageNs': now - self.acquired_ns if self.acquired_ns is not None else None,
                'origin': self.origin, 'availability': state}


def finite(value, low, high):
    return type(value) in (int, float) and low <= value <= high and math.isfinite(value)


def position_valid(value):
    return (isinstance(value, (tuple, list)) and len(value) == 3 and
            finite(value[0], -90, 90) and finite(value[1], -180, 180) and
            finite(value[2], 0, 10000) and value[2] > 0)


def xy(position, anchor):
    # Short local track projection, wrapping longitude at the date line.
    longitude = (position[1] - anchor[1] + 180) % 360 - 180
    return (math.radians(longitude) * EARTH * math.cos(math.radians(anchor[0])),
            math.radians(position[0] - anchor[0]) * EARTH)


def distance(a, b):
    return math.hypot(*xy(a, b))


class AccelerationSummary:
    """Covered linear estimates, independent of current g and historical peaks.

    The enclosing engine admits observations atomically. This helper owns one
    endpoint and three compensated integrals, with no history or persistence.
    """
    MAXIMUM_SPAN_NS = 86400 * NS
    COMPONENT_BOUND_G = 100 / G
    ROUNDING_TOLERANCE = 1e-12

    def __init__(self, maximum_gap_ns):
        self.maximum_gap_ns = maximum_gap_ns
        self.reset_floor = 0
        self.begin_generation()

    def begin_generation(self):
        self.origin = None
        self.previous = None
        self.acquisition_floor = 0
        self.first_baseline_ns = None
        self.first_ns = self.last_ns = None
        self.covered_ns = 0
        self.integrals = ((0.0, 0.0), (0.0, 0.0), (0.0, 0.0))
        self.partial = False
        self.limit_reached = False
        self.reason = 'Need new fresh acceleration'

    def reset(self, now):
        self.begin_generation()
        # An action date is only an admission floor, never an acquisition.
        self.reset_floor = now

    def _break(self, reason):
        if self.first_baseline_ns is not None:
            self.partial = True
        self.previous = None
        if not self.limit_reached:
            self.reason = reason

    def _limit(self, reason):
        self._break(reason)
        self.limit_reached = True
        self.reason = reason

    @staticmethod
    def _host_date(value):
        return type(value) is int and 0 < value <= 2**63 - 1

    def advance(self, acceleration, now):
        if not self._host_date(now):
            self._limit('Summary clock outside signed host domain')
            return
        if (not acceleration or not acceleration.fresh(now) or
                self.previous is not None and not self.previous.fresh(now)):
            self._break('Acceleration unavailable or stale')

    @staticmethod
    def _add(pair, value):
        # Neumaier compensation retains signed cancellation without a list.
        total, correction = pair
        updated = total + value
        if abs(total) >= abs(value):
            correction += (total - updated) + value
        else:
            correction += (value - updated) + total
        return updated, correction

    def accept(self, obs, now):
        if not self._host_date(now) or not self._host_date(obs.acquired_ns):
            self._limit('Summary clock outside signed host domain')
            return
        if obs.acquired_ns <= max(self.acquisition_floor, self.reset_floor):
            return
        if self.origin is not None and self.origin != obs.origin:
            self.begin_generation()
        self.origin = obs.origin
        if self.limit_reached:
            return
        self.acquisition_floor = obs.acquired_ns
        if self.first_baseline_ns is None:
            self.first_baseline_ns = obs.acquired_ns
        if obs.acquired_ns - self.first_baseline_ns > self.MAXIMUM_SPAN_NS:
            self._limit('Summary limit reached; new sport session required')
            return
        previous = self.previous
        if previous is not None:
            interval = obs.acquired_ns - previous.acquired_ns
            continuous = (0 < interval <= self.maximum_gap_ns and
                          interval < min(previous.maximum_age_ns, obs.maximum_age_ns) and
                          previous.fresh(now))
            if continuous:
                covered = self.covered_ns + interval
                if covered > self.MAXIMUM_SPAN_NS:
                    self._limit('Summary limit reached; new sport session required')
                    return
                x0, y0 = (component / G for component in previous.value)
                x1, y1 = (component / G for component in obs.value)
                seconds = interval / NS
                # Exact integral of the squared linear vector in real arithmetic:
                # (|u|² + u·v + |v|²)/3 = (|u|² + |v|² + |u+v|²)/6.
                energy = (x0*x0 + y0*y0 + x1*x1 + y1*y1 +
                          (x0+x1)**2 + (y0+y1)**2) / 6
                additions = (seconds * (x0+x1) / 2,
                             seconds * (y0+y1) / 2, seconds * energy)
                staged = tuple(self._add(pair, value)
                               for pair, value in zip(self.integrals, additions))
                totals = tuple(total + correction for total, correction in staged)
                duration = covered / NS
                bound = self.COMPONENT_BOUND_G
                mean_tolerance = self.ROUNDING_TOLERANCE * max(1, bound)
                energy_bound = 2 * bound * bound
                energy_tolerance = self.ROUNDING_TOLERANCE * max(1, energy_bound)
                if (not all(math.isfinite(value) for pair in staged for value in pair) or
                        not all(math.isfinite(value) for value in totals) or
                        abs(totals[0] / duration) > bound + mean_tolerance or
                        abs(totals[1] / duration) > bound + mean_tolerance or
                        not 0 <= totals[2] / duration <= energy_bound + energy_tolerance):
                    self._limit('Summary numeric bound failed; new sport session required')
                    return
                # Commit all three integrals, duration and contributing dates together.
                self.integrals = staged
                self.covered_ns = covered
                if self.first_ns is None:
                    self.first_ns = previous.acquired_ns
                self.last_ns = obs.acquired_ns
                self.reason = None
            else:
                self._break('Acceleration acquisition gap')
        self.previous = obs

    def model(self, session_id, generation, acceleration, now):
        duration = self.covered_ns / NS if self.covered_ns else None
        totals = tuple(total + correction for total, correction in self.integrals)
        return {'version': 1, 'authority': 'software-acceleration-summary',
                'method': 'piecewise-linear-acquisition', 'persistence': 'process-memory',
                'sessionId': session_id, 'sourceGeneration': generation, 'origin': self.origin,
                'maximumGapNs': self.maximum_gap_ns, 'maximumSpanNs': self.MAXIMUM_SPAN_NS,
                'phase': 'limited' if self.limit_reached else 'collecting' if self.previous else 'waiting',
                'reason': self.reason, 'inputAvailable': bool(acceleration and acceleration.fresh(now)),
                'firstAcquiredNs': self.first_ns, 'lastAcquiredNs': self.last_ns,
                'coveredSeconds': duration,
                'meanLongitudinalG': totals[0] / duration if duration else None,
                'meanLateralG': totals[1] / duration if duration else None,
                'rmsMagnitudeG': math.sqrt(totals[2] / duration) if duration else None,
                'partial': self.partial, 'limitReached': self.limit_reached}


class RecentPace:
    """Live-only bounded original intervals, anchored to admitted acquisitions."""
    WINDOW_NS = 60 * NS
    INTERVAL_CAPACITY = 4096

    def __init__(self):
        self.intervals = deque()
        self.first_ns = None
        self.anchor_ns = None
        self.lost_until_ns = None

    def observe(self, obs):
        if self.first_ns is None:
            self.first_ns = obs.acquired_ns
        self.anchor_ns = obs.acquired_ns
        cutoff = self.anchor_ns - self.WINDOW_NS
        while self.intervals and self.intervals[0][1].acquired_ns <= cutoff:
            self.intervals.popleft()
        if self.lost_until_ns is not None and self.lost_until_ns <= cutoff:
            self.lost_until_ns = None

    def accept_interval(self, old, obs):
        # Called only after all existing Trip interval and capacity decisions.
        if len(self.intervals) == self.INTERVAL_CAPACITY:
            _, lost = self.intervals.popleft()
            self.lost_until_ns = lost.acquired_ns
        self.intervals.append((old, obs))

    def model(self, generation, origin, current_input_available):
        anchor = self.anchor_ns
        cutoff = anchor - self.WINDOW_NS if anchor is not None else None
        covered_ns = 0
        distances = []
        for old, new in self.intervals:
            start = max(old.acquired_ns, cutoff)
            duration = new.acquired_ns - start
            # The cutoff is an integration boundary, never an acquisition.
            fraction = (start - old.acquired_ns) / (new.acquired_ns - old.acquired_ns)
            start_speed = old.value + (new.value - old.value) * fraction
            covered_ns += duration
            distances.append((start_speed + new.value) * .5 * (duration / NS) / 3600)
        known = covered_ns > 0
        distance_km = math.fsum(distances) if known else None
        span_ns = min(self.WINDOW_NS, anchor - self.first_ns) if anchor is not None else None
        limited = self.lost_until_ns is not None
        return {'version': 1, 'authority': 'software-speed-estimate',
                'method': 'linear-speed-trapezoids', 'nominalWindowSeconds': 60,
                'intervalCapacity': self.INTERVAL_CAPACITY, 'retainedIntervals': len(self.intervals),
                'sourceGeneration': generation, 'origin': origin,
                'anchorAcquiredNs': anchor,
                'derivedWindowStartNs': max(0, cutoff) if cutoff is not None else None,
                'derivedWindowEndNs': anchor,
                'firstContributingAcquiredNs': (self.intervals[0][0].acquired_ns
                                                if self.intervals else None),
                'lastContributingAcquiredNs': (self.intervals[-1][1].acquired_ns
                                               if self.intervals else None),
                'distanceKm': distance_km,
                'coveredSeconds': covered_ns / NS if known else None,
                'averageSpeedKmh': distance_km * 3600 * NS / covered_ns if known else None,
                'observedSpanSeconds': span_ns / NS if span_ns is not None else None,
                'uncoveredSeconds': (span_ns - covered_ns) / NS if span_ns is not None else None,
                'coverageRatio': covered_ns / self.WINDOW_NS if anchor is not None else None,
                'partial': covered_ns < self.WINDOW_NS or limited,
                'limited': limited, 'currentInputAvailable': current_input_available}


class TripStatistics:
    """Software journey estimates from admitted speed; never a vehicle TripState.

    The enclosing SportEngine owns atomic validation and acquisition ordering.
    Only fresh changed observations reach accept(), not polling or heartbeats.
    History is bounded memory data; the adapter may persist labelled archives.
    """
    HISTORY_LIMIT = 16
    MAXIMUM_DURATION_NS = 315_576_000 * NS
    MAXIMUM_DISTANCE_KM = 9_999_999
    MOVING_THRESHOLD_KMH = 0.5
    MAXIMUM_GAP_NS = 250_000_000
    SPEED_BANDS_KMH = ((0, 30), (30, 60), (60, 90), (90, 120), (120, None))

    def __init__(self):
        self.history = deque(maxlen=self.HISTORY_LIMIT)
        self.journey_id = 1
        self.generation = None
        self.acquisition_floor = 0
        self.reset_floor = 0
        self._clear()

    def _clear(self):
        self.recent = RecentPace()
        self.origin = None
        self.maximum_age_ns = None
        self.first_ns = self.last_ns = None
        self.previous = None
        self.covered_ns = 0
        self.speed_band_ns = [0.0] * len(self.SPEED_BANDS_KMH)
        self.speed_band_distance_km = [0.0] * len(self.SPEED_BANDS_KMH)
        self.speed_anchor = None
        self.speed_mean_offset = 0.0
        self.speed_m2_ns = 0.0
        self.moving_ns = 0.0
        self.stationary_run_ns = 0.0
        self.longest_stationary_ns = 0.0
        self.distance_km = self.moving_distance_km = 0.0
        self.peak = None
        self.has_gaps = False
        self.reason = 'Need new fresh speed'
        self.limit_reached = False

    def _statistics(self):
        span_ns = self.last_ns - self.first_ns if self.first_ns is not None else None
        known = self.covered_ns > 0
        return {'journeyId': self.journey_id, 'sourceGeneration': self.generation,
                'origin': self.origin, 'speedMaximumAgeNs': self.maximum_age_ns, 'firstAcquiredNs': self.first_ns,
                'lastAcquiredNs': self.last_ns,
                'distanceKm': self.distance_km if known else None,
                'coveredSeconds': self.covered_ns / NS if known else None,
                'movingSeconds': self.moving_ns / NS if known else None,
                'movingDistanceKm': self.moving_distance_km if known else None,
                'averageSpeedKmh': self.distance_km * 3600 * NS / self.covered_ns if known else None,
                'movingAverageSpeedKmh': (self.moving_distance_km * 3600 * NS / self.moving_ns
                                          if self.moving_ns > 0 else None),
                'peak': copy.deepcopy(self.peak),
                'acquiredSpanSeconds': span_ns / NS if span_ns is not None else None,
                'uncoveredSeconds': (span_ns - self.covered_ns) / NS if span_ns is not None else None,
                'coverageRatio': self.covered_ns / span_ns if span_ns else None,
                'partial': self.has_gaps, 'limitReached': self.limit_reached}

    def _archive(self, reason, now):
        if self.first_ns is not None:
            self.history.append({**self._statistics(), 'endedReason': reason,
                                 'archivedAtNs': now})
        self.journey_id += 1
        self._clear()

    def retire(self, now):
        self._archive('Sport owner retired', now)

    def begin_generation(self, generation, now):
        if self.generation is not None:
            self._archive('Source generation changed', now)
        self.generation = generation
        self.acquisition_floor = 0

    def reset(self, now):
        self._archive('User software reset', now)
        # Queued pre-action acquisitions and the owner's cached input must not
        # become a new baseline. This is an action date, not a measurement date.
        self.reset_floor = now

    def advance(self, speed, now):
        if not speed or not speed.fresh(now):
            if self.previous is not None:
                self.has_gaps = True
            self.previous = None
            self.stationary_run_ns = 0.0
            self.reason = 'Speed unavailable or stale'

    def accept(self, obs, now):
        if obs.acquired_ns <= max(self.acquisition_floor, self.reset_floor):
            return
        self.acquisition_floor = obs.acquired_ns
        if self.origin is not None and obs.origin != self.origin:
            self._archive('Speed origin changed', now)
        self.origin = obs.origin
        self.maximum_age_ns = obs.maximum_age_ns
        if self.limit_reached:
            return
        old = self.previous
        if self.first_ns is None:
            self.first_ns = obs.acquired_ns
        self.last_ns = obs.acquired_ns
        self.recent.observe(obs)
        if self.peak is None or obs.value > self.peak['speedKmh']:
            self.peak = {'speedKmh': obs.value, 'acquiredNs': obs.acquired_ns,
                         'origin': obs.origin}
        self.reason = None
        if obs.acquired_ns - self.first_ns > self.MAXIMUM_DURATION_NS:
            self.limit_reached = self.has_gaps = True
            self.previous = None
            self.stationary_run_ns = 0.0
            self.reason = 'Software statistics limit reached; reset required'
            return
        if old is not None:
            interval = obs.acquired_ns - old.acquired_ns
            # Strictly inside both age policies: exact expiry cannot be a
            # continuous interval. Longer gaps contribute only to span.
            continuous = (interval <= self.MAXIMUM_GAP_NS and
                          interval < min(old.maximum_age_ns, obs.maximum_age_ns))
            if continuous:
                seconds = interval / NS
                distance_km = (old.value + obs.value) * .5 * seconds / 3600
                if (self.covered_ns + interval > self.MAXIMUM_DURATION_NS or
                        self.distance_km + distance_km > self.MAXIMUM_DISTANCE_KM):
                    self.limit_reached = self.has_gaps = True
                    self.previous = None
                    self.stationary_run_ns = 0.0
                    self.reason = 'Software statistics limit reached; reset required'
                    return
                # Merge linear-segment population moments by original
                # duration. A fixed speed anchor preserves sub-ULP mean offsets
                # for nearly constant high speeds; central M2 avoids subtracting
                # close raw squared totals. Every M2 contribution is nonnegative.
                if self.speed_anchor is None:
                    self.speed_anchor = old.value
                segment_mean_offset = ((old.value - self.speed_anchor) +
                                       (obs.value - self.speed_anchor)) * .5
                segment_variance = (obs.value - old.value) ** 2 / 12
                combined_ns = self.covered_ns + interval
                delta = segment_mean_offset - self.speed_mean_offset
                self.speed_m2_ns += interval * (segment_variance +
                                               delta * delta * (self.covered_ns / combined_ns))
                self.speed_mean_offset += delta * (interval / combined_ns)
                # Linear interpolation within the admitted segment. Integrate
                # only the portion strictly above the explicit moving band.
                lower, upper = 0.0, 1.0
                threshold = self.MOVING_THRESHOLD_KMH
                if old.value <= threshold and obs.value <= threshold:
                    upper = 0.0
                elif old.value <= threshold:
                    lower = (threshold - old.value) / (obs.value - old.value)
                elif obs.value <= threshold:
                    upper = (threshold - old.value) / (obs.value - old.value)
                fraction = upper - lower
                # Live-only low-speed runs use the same admitted linear
                # interval. Any above-threshold portion separates runs; a
                # threshold endpoint alone contributes no duration.
                if old.value <= threshold:
                    self.stationary_run_ns += interval * (1.0 if obs.value <= threshold else lower)
                else:
                    self.stationary_run_ns = (interval * (threshold - obs.value) / (old.value - obs.value)
                                              if obs.value <= threshold else 0.0)
                self.longest_stationary_ns = max(self.longest_stationary_ns, self.stationary_run_ns)
                if obs.value > threshold:
                    self.stationary_run_ns = 0.0
                start_speed = old.value + (obs.value - old.value) * lower
                end_speed = old.value + (obs.value - old.value) * upper
                self.recent.accept_interval(old, obs)
                self.covered_ns += interval
                # Live journey only: the fixed ranges classify the same
                # original covered interval, never a poll or archived record.
                low_speed, high_speed = sorted((old.value, obs.value))
                for index, (low, high) in enumerate(self.SPEED_BANDS_KMH):
                    if low_speed == high_speed:
                        band_fraction = float(low <= low_speed and
                                              (high is None or low_speed < high))
                    else:
                        overlap = max(0.0, min(high_speed, high if high is not None else high_speed) -
                                      max(low_speed, low))
                        band_fraction = overlap / (high_speed - low_speed)
                    self.speed_band_ns[index] += interval * band_fraction
                    if band_fraction > 0:
                        # Integrate the original linear speed over this band's
                        # covered portion, using its clipped endpoint mean.
                        band_start = max(low_speed, low)
                        band_end = min(high_speed, high) if high is not None else high_speed
                        self.speed_band_distance_km[index] += (band_start + band_end) * .5 * seconds * band_fraction / 3600
                self.distance_km += distance_km
                self.moving_ns += interval * fraction
                self.moving_distance_km += (start_speed + end_speed) * .5 * seconds * fraction / 3600
            else:
                self.stationary_run_ns = 0.0
                self.has_gaps = True
                self.reason = 'Speed acquisition gap'
        self.previous = obs

    def model(self, current_input_available=False):
        deviation = math.sqrt(self.speed_m2_ns / self.covered_ns) if self.covered_ns else None
        rms = (math.hypot(self.speed_anchor + self.speed_mean_offset, deviation)
               if self.covered_ns else None)
        return {'recent': self.recent.model(self.generation, self.origin, current_input_available),
                'version': 1, 'authority': 'software-speed-estimate',
                'method': 'linear-speed-trapezoids', 'maximumGapNs': self.MAXIMUM_GAP_NS,
                'movingThresholdKmh': self.MOVING_THRESHOLD_KMH,
                'historyLimit': self.HISTORY_LIMIT, 'persistence': 'process-memory',
                'phase': 'limited' if self.limit_reached else 'collecting' if self.previous else 'waiting',
                'reason': self.reason, **self._statistics(),
                'timeWeightedRmsSpeedKmh': rms, 'speedStandardDeviationKmh': deviation,
                'coveredStationarySeconds': (self.covered_ns - self.moving_ns) / NS if self.covered_ns else None,
                'longestContinuousStationarySeconds': self.longest_stationary_ns / NS if self.covered_ns else None,
                'speedBands': [{'lowerKmh': low, 'upperKmh': high,
                                'seconds': self.speed_band_ns[index] / NS if self.covered_ns else None,
                                'distanceKm': self.speed_band_distance_km[index] if self.covered_ns else None}
                               for index, (low, high) in enumerate(self.SPEED_BANDS_KMH)],
                'history': copy.deepcopy(list(self.history))}


class Rolling100To200:
    """One live rolling estimate, independent of the original timers/archive.

    Only atomically admitted fresh changed speeds reach accept(). No polling
    date becomes a crossing or acquired elapsed time. The cutoff survives
    generation/provenance loss; session reset requires post-action acquisitions.
    """
    def __init__(self, maximum_gap_ns, lifetime_ns):
        self.maximum_gap_ns = maximum_gap_ns
        self.lifetime_ns = lifetime_ns
        self.acquisition_floor = 0
        self.origin = None
        self.previous = None
        self.current_run = None
        self.last_result = None
        self.phase = 'waiting'
        self.reason = 'Need acquired speed at or below 98 km/h'
        self.elapsed_seconds = None
        self.retired = False

    def invalidate(self, reason):
        if self.retired:
            return
        self.phase = 'invalid' if self.current_run is not None or self.phase == 'invalid' else 'waiting'
        self.reason = reason
        self.previous = None
        self.current_run = None
        self.elapsed_seconds = None

    def reset_session(self, now):
        self.invalidate('Session ended; need post-reset rearm')
        self.acquisition_floor = max(self.acquisition_floor, now)
        self.last_result = None
        self.origin = None

    def retire(self):
        self.invalidate('Sport acquisition owner retired')
        self.retired = True
        self.phase = 'retired'
        self.reason = 'Sport acquisition owner retired'

    def advance(self, speed, now):
        if not speed or not speed.fresh(now):
            self.invalidate('Speed unavailable or stale')
        elif self.current_run is not None and now - self.current_run['startCrossing']['derivedNs'] >= self.lifetime_ns:
            self.invalidate('Timeout')

    @staticmethod
    def crossing(old, obs, threshold):
        def endpoint(value):
            return {'speedKmh': value.value, 'acquiredNs': value.acquired_ns,
                    'maximumAgeNs': value.maximum_age_ns, 'origin': value.origin}
        # Add the rounded relative offset as an integer. Converting the original
        # absolute date to float would lose nanoseconds at large host dates.
        offset = round((obs.acquired_ns - old.acquired_ns) *
                       (threshold - old.value) / (obs.value - old.value))
        return {'derivedNs': old.acquired_ns + offset,
                'before': endpoint(old), 'after': endpoint(obs)}

    def accept(self, obs, now, generation, session_id):
        if self.retired or obs.acquired_ns <= self.acquisition_floor:
            return
        self.acquisition_floor = obs.acquired_ns
        old = self.previous
        if self.origin is not None and obs.origin != self.origin:
            self.invalidate('Speed provenance changed')
            old = None
        self.origin = obs.origin
        if old is not None:
            interval = obs.acquired_ns - old.acquired_ns
            if (not old.fresh(now) or interval > self.maximum_gap_ns or
                    interval >= min(old.maximum_age_ns, obs.maximum_age_ns)):
                self.invalidate('Speed acquisition gap or expired predecessor')
                old = None
        if obs.value <= 98:
            self.current_run = None
            self.elapsed_seconds = None
            self.phase = 'armed'
            self.reason = None
        if old is not None and old.value < 100 <= obs.value and self.phase == 'armed':
            self.current_run = {'sessionId': session_id, 'sourceGeneration': generation,
                                'origin': obs.origin, 'startCrossing': self.crossing(old, obs, 100),
                                'lastAcquiredNs': obs.acquired_ns}
            self.phase = 'running'
            self.reason = None
        if self.current_run is not None:
            run = self.current_run
            if old is not None and old.value < 200 <= obs.value:
                end = self.crossing(old, obs, 200)
                duration = end['derivedNs'] - run['startCrossing']['derivedNs']
                if duration <= 0:
                    self.invalidate('Insufficient acquisition resolution')
                    return
                self.last_result = {'sessionId': run['sessionId'],
                                    'sourceGeneration': run['sourceGeneration'], 'origin': run['origin'],
                                    'startCrossing': run['startCrossing'], 'endCrossing': end,
                                    'seconds': duration / NS}
                self.current_run = None
                self.elapsed_seconds = duration / NS
                self.phase = 'complete'
            else:
                run['lastAcquiredNs'] = obs.acquired_ns
                self.elapsed_seconds = (obs.acquired_ns - run['startCrossing']['derivedNs']) / NS
        self.previous = obs

    def model(self):
        return {'version': 1, 'authority': 'software-speed-estimate',
                'method': 'linear-speed-interpolation', 'startSpeedKmh': 100,
                'endSpeedKmh': 200, 'rearmSpeedKmh': 98,
                'maximumGapNs': self.maximum_gap_ns, 'maximumRunNs': self.lifetime_ns,
                'phase': self.phase, 'reason': self.reason,
                'elapsedSeconds': self.elapsed_seconds,
                'currentRun': copy.deepcopy(self.current_run),
                'lastResult': copy.deepcopy(self.last_result)}


class SportEngine:
    TIMER_LIMIT_NS = 120 * NS
    HISTORY_LIMIT = 64
    TRACE_LIMIT = 2048

    def __init__(self, max_gap_ns=250_000_000, gps_gap_ns=500_000_000):
        if not 0 < max_gap_ns <= NS or not 0 < gps_gap_ns <= 2 * NS:
            raise ValueError('Invalid sport continuity limits')
        self.max_gap_ns = max_gap_ns
        self.gps_gap_ns = gps_gap_ns
        self.trip = TripStatistics()
        self.acceleration_summary = AccelerationSummary(max_gap_ns)
        self.rolling100_to_200 = Rolling100To200(max_gap_ns, self.TIMER_LIMIT_NS)
        self.inputs = {}
        self.watermarks = {}
        self.generation = None
        self.last_now = 0
        self.session_id = 1
        self.session_started = None
        self.history = deque(maxlen=self.HISTORY_LIMIT)
        self.revision = 0
        self.line = None
        self.peaks = {'longitudinalPositiveG': None, 'longitudinalNegativeG': None,
                      'lateralPositiveG': None, 'lateralNegativeG': None, 'absoluteG': None}
        self.peak_sources = set()
        self.peak_observations = {}
        self._reset_active()

    def _reset_active(self):
        self.previous_speed = None
        self.stationary_since = None
        self.launch_armed = False
        self.rolling_armed = False
        self.timers = {key: {'phase': 'waiting', 'elapsedSeconds': None,
                             'result': None, 'reason': 'Need fresh speed'}
                       for key in ('0-100', '50-80', '0-200', 'quarter-mile')}
        self.runs = {}
        self.previous_gps = None
        self.lap_started = None
        self.lap_left_gate = False
        self.lap_distance = 0.0
        self.trace = []
        self.trace_overflow = False
        self.best_trace = []
        self.last_lap = None
        self.best_lap = None
        self.lap_delta = None
        self.reference_progress = None
        self.lap_reason = 'Set a start/finish line' if self.line is None else 'Waiting for GPS crossing'

    def _event(self, kind, **data):
        event = {'sessionId': self.session_id, 'sourceGeneration': self.generation,
                 'kind': kind, **data}
        self.history.append(event)
        self.revision += 1
        return event

    def _abort_speed(self, reason):
        for name in tuple(self.runs):
            self._event('aborted', timer=name, reason=reason, atNs=self.last_now)
            self.timers[name].update(phase='invalid', elapsedSeconds=None, result=None, reason=reason)
        self.runs.clear()
        for state in self.timers.values():
            if state['phase'] in ('armed', 'waiting'):
                state.update(phase='waiting', elapsedSeconds=None, reason=reason)
        self.previous_speed = None
        self.stationary_since = None
        self.launch_armed = False
        self.rolling_armed = False

    def _abort_lap(self, reason):
        if self.lap_started is not None:
            self._event('aborted', timer='lap', reason=reason, atNs=self.last_now)
        self.lap_started = None
        self.previous_gps = None
        self.trace = []
        self.lap_delta = None
        self.reference_progress = None
        self.lap_reason = reason
        self.lap_left_gate = False
        self.lap_distance = 0.0

    def advance(self, now):
        if type(now) is not int or now <= 0 or now < self.last_now:
            raise ValueError('Sport host time reversed')
        self.last_now = now
        self.acceleration_summary.advance(self.inputs.get('acceleration'), now)
        speed = self.inputs.get('speed_kmh')
        self.trip.advance(speed, now)
        self.rolling100_to_200.advance(speed, now)
        if not speed or not speed.fresh(now):
            self._abort_speed('Speed unavailable or stale')
        gps = self.inputs.get('gps_position')
        if not gps or not gps.fresh(now):
            self._abort_lap('GPS unavailable or stale')
        for name, run in tuple(self.runs.items()):
            if now - run['start'] >= self.TIMER_LIMIT_NS:
                self._event('aborted', timer=name, reason='Timeout', atNs=now)
                self.timers[name].update(phase='invalid', elapsedSeconds=None, reason='Timeout')
                del self.runs[name]

    def accept(self, generation, observations, now):
        if type(generation) is not int or not 0 < generation <= 2**64 - 1 or generation < (self.generation or 0):
            raise ValueError('Sport source generation reversed')
        if type(now) is not int or now <= 0 or now < self.last_now:
            raise ValueError('Sport host time reversed')
        if not isinstance(observations, dict) or set(observations) - {'speed_kmh', 'acceleration', 'gps_position'}:
            raise ValueError('Unknown sport observation')
        observations = dict(observations)
        new_generation = generation != self.generation
        marks = {} if new_generation else self.watermarks.copy()
        changed = []
        for key, obs in observations.items():
            if not isinstance(obs, Observation) or not obs.valid(now):
                raise ValueError('Invalid sport observation metadata')
            if obs.value is not None:
                if key == 'speed_kmh' and not finite(obs.value, 0, 300):
                    raise ValueError('Invalid speed')
                if key == 'acceleration' and not (isinstance(obs.value, (tuple, list)) and
                        len(obs.value) == 2 and all(finite(v, -100, 100) for v in obs.value)):
                    raise ValueError('Invalid acceleration')
                if key == 'gps_position' and not position_valid(obs.value):
                    raise ValueError('Invalid GPS')
            if obs.value is not None and key in ('acceleration', 'gps_position'):
                obs = Observation(tuple(obs.value), obs.acquired_ns, obs.maximum_age_ns,
                                  obs.origin, obs.availability)
                observations[key] = obs
            old = marks.get(key)
            policy = self.watermarks.get(key)
            if policy and obs.maximum_age_ns != policy.maximum_age_ns:
                raise ValueError('Sport age policy changed')
            if old:
                if obs.maximum_age_ns != old.maximum_age_ns:
                    raise ValueError('Sport age policy changed')
                if old.acquired_ns is not None and (obs.acquired_ns is None or obs.acquired_ns < old.acquired_ns):
                    raise ValueError('Sport acquisition reversed')
                if obs.acquired_ns == old.acquired_ns:
                    if obs == old:
                        continue
                    # Withdrawal may keep an acquisition watermark; a new value cannot.
                    if not (obs.value is None and obs.origin == old.origin and
                            obs.availability in ('stale', 'disconnected') or
                            obs.value is None and old.value is None and obs.origin == old.origin):
                        raise ValueError('Conflicting or revived acquisition')
            marks[key] = obs
            changed.append(key)
        if new_generation:
            self.acceleration_summary.begin_generation()
            self.trip.begin_generation(generation, now)
            self.rolling100_to_200.invalidate('Source generation changed')
            self.last_now = now
            self._abort_speed('Source generation changed')
            self._abort_lap('Source generation changed')
            self.inputs.clear()
            self.best_lap = self.last_lap = None
            self.best_trace = []
        self.generation = generation
        self.watermarks = marks
        self.inputs.update({key: observations[key] for key in changed})
        if self.session_started is None:
            self.session_started = now
        self.advance(now)
        for key in changed:
            obs = self.inputs[key]
            if not obs.fresh(now):
                continue
            if key == 'speed_kmh':
                self.rolling100_to_200.accept(obs, now, generation, self.session_id)
                self.trip.accept(obs, now)
                self._speed(obs)
            elif key == 'acceleration':
                self._acceleration(obs)
                self.acceleration_summary.accept(obs, now)
            elif obs.value[2] <= 5:
                self._gps(obs)
            else:
                self._abort_lap('GPS accuracy exceeds 5 m')

    def _acceleration(self, obs):
        longitudinal, lateral = (v / G for v in obs.value)
        values = {'longitudinalPositiveG': longitudinal, 'longitudinalNegativeG': longitudinal,
                  'lateralPositiveG': lateral, 'lateralNegativeG': lateral,
                  'absoluteG': math.hypot(longitudinal, lateral)}
        for key, value in values.items():
            if 'Positive' in key and value < 0 or 'Negative' in key and value > 0:
                continue
            old = self.peaks[key]
            if old is None or (value < old if 'Negative' in key else value > old):
                self.peaks[key] = value
                self.peak_observations[key] = {'g': value, 'acquiredNs': obs.acquired_ns, 'origin': obs.origin}
        self.peak_sources.add(obs.origin)

    @staticmethod
    def _cross_time(a, b, threshold):
        t0, v0 = a.acquired_ns, a.value
        return round(t0 + (b.acquired_ns - t0) * (threshold - v0) / (b.value - v0))

    def _start(self, name, at, origin):
        self.runs[name] = {'start': at, 'last': at, 'distance': 0.0, 'origin': origin}
        self.timers[name].update(phase='running', elapsedSeconds=0.0, result=None, reason=None)

    def _finish(self, name, at, run):
        result = self._event('acceleration', timer=name, startNs=run['start'], endNs=at,
                             seconds=(at - run['start']) / NS, origin=run['origin'],
                             method='linear-speed-interpolation',
                             maximumGapNs=self.max_gap_ns)
        self.timers[name].update(phase='complete', elapsedSeconds=result['seconds'], result=result, reason=None)
        del self.runs[name]

    def _speed(self, obs):
        old = self.previous_speed
        if old and (obs.origin != old.origin or obs.acquired_ns - old.acquired_ns >
                    min(self.max_gap_ns, old.maximum_age_ns, obs.maximum_age_ns)):
            self._abort_speed('Speed gap or provenance change')
            old = None
        if obs.value <= 0.5:
            if self.stationary_since is None:
                self.stationary_since = obs.acquired_ns
            if obs.acquired_ns - self.stationary_since >= NS and not self.runs:
                self.launch_armed = True
                for name in ('0-100', '0-200', 'quarter-mile'):
                    self.timers[name].update(phase='armed', elapsedSeconds=None, reason=None)
        else:
            self.stationary_since = None
        if obs.value <= 48 and '50-80' not in self.runs:
            self.rolling_armed = True
            self.timers['50-80'].update(phase='armed', elapsedSeconds=None, reason=None)
        if old and obs.value > old.value:
            if self.launch_armed and old.value <= 0.5 < obs.value:
                # Launch date is last acquired rest sample; no invented zero crossing.
                for name in ('0-100', '0-200', 'quarter-mile'):
                    self._start(name, old.acquired_ns, obs.origin)
                self.launch_armed = False
            if self.rolling_armed and old.value < 50 <= obs.value:
                self._start('50-80', self._cross_time(old, obs, 50), obs.origin)
                self.rolling_armed = False
        if old:
            dt = (obs.acquired_ns - old.acquired_ns) / NS
            for name, run in tuple(self.runs.items()):
                threshold = {'0-100': 100, '50-80': 80, '0-200': 200}.get(name)
                if threshold is not None and old.value < threshold <= obs.value:
                    self._finish(name, self._cross_time(old, obs, threshold), run)
                    continue
                if name == 'quarter-mile':
                    v0, v1 = old.value / 3.6, obs.value / 3.6
                    segment = (v0 + v1) * dt / 2
                    remaining = 402.336 - run['distance']
                    if segment >= remaining and segment > 0:
                        acceleration = (v1 - v0) / dt
                        # Stable root of v0*t + 0.5*a*t² = remaining.
                        denominator = v0 + math.sqrt(max(0, v0 * v0 + 2 * acceleration * remaining))
                        elapsed = 2 * remaining / denominator if denominator else dt
                        self._finish(name, old.acquired_ns + round(elapsed * NS), run)
                        continue
                    run['distance'] += segment
                if obs.value <= 0.5:
                    self._event('aborted', timer=name, reason='Returned to rest', atNs=obs.acquired_ns)
                    self.timers[name].update(phase='invalid', elapsedSeconds=None, result=None,
                                             reason='Returned to rest')
                    del self.runs[name]
                else:
                    run['last'] = obs.acquired_ns
                    self.timers[name]['elapsedSeconds'] = (obs.acquired_ns - run['start']) / NS
        self.previous_speed = obs

    def set_line(self, a, b):
        if not (isinstance(a, (tuple, list)) and isinstance(b, (tuple, list)) and
                len(a) == len(b) == 2 and
                position_valid([*a, 1]) and position_valid([*b, 1]) and
                abs(a[0]) < 85 and abs(b[0]) < 85 and 5 <= distance(a, b) <= 200):
            raise ValueError('Line requires WGS84 endpoints 5–200 m apart below 85 degrees latitude')
        self.line = [list(a), list(b)]
        self._abort_lap('Start/finish line changed')
        self.best_lap = self.last_lap = None
        self.best_trace = []
        self.revision += 1

    def _gate(self, position):
        a, b = self.line
        x, y = xy(position, a)
        bx, by = xy(b, a)
        length = math.hypot(bx, by)
        return (bx * y - by * x) / length, (x * bx + y * by) / (length * length)

    def _gps(self, obs):
        if self.line is None:
            return
        if self.best_lap is not None and self.best_lap['origin'] != obs.origin:
            self.best_lap = self.last_lap = None
            self.best_trace = []
        old = self.previous_gps
        if old:
            gap = obs.acquired_ns - old.acquired_ns
            segment = distance(obs.value, old.value)
            if (obs.origin != old.origin or gap > min(self.gps_gap_ns, old.maximum_age_ns,
                    obs.maximum_age_ns) or segment > 90 * gap / NS + obs.value[2] + old.value[2]):
                self._abort_lap('GPS gap, jump or provenance change')
                old = None
        side, along = self._gate(obs.value)
        if self.lap_started is not None and abs(side) >= 20:
            self.lap_left_gate = True
        crossing = None
        if old:
            old_side, old_along = self._gate(old.value)
            if old_side < 0 <= side:
                fraction = -old_side / (side - old_side)
                crossing_along = old_along + (along - old_along) * fraction
                if 0 <= crossing_along <= 1:
                    crossing = old.acquired_ns + round((obs.acquired_ns - old.acquired_ns) * fraction)
            if self.lap_started is not None:
                self.lap_distance += distance(obs.value, old.value)
        if crossing is not None and (self.lap_started is None or
                self.lap_left_gate and crossing - self.lap_started >= 10 * NS):
            if self.lap_started is not None:
                result = self._event('lap', line=copy.deepcopy(self.line), startNs=self.lap_started, endNs=crossing,
                                     seconds=(crossing - self.lap_started) / NS, origin=obs.origin,
                                     method='GPS-directional-line-interpolation',
                                     maximumGapNs=self.gps_gap_ns)
                self.last_lap = result
                if self.best_lap is None or result['seconds'] < self.best_lap['seconds']:
                    self.best_lap = result
                    self.best_trace = [] if self.trace_overflow else self.trace.copy()
            self.lap_started = crossing
            self.lap_left_gate = False
            self.lap_distance = 0.0
            self.trace = []
            self.trace_overflow = False
            self.lap_delta = None
            self.reference_progress = None
        if self.lap_started is not None:
            if not self.trace_overflow and len(self.trace) < self.TRACE_LIMIT:
                self.trace.append((tuple(obs.value), (obs.acquired_ns - self.lap_started) / NS))
            else:
                # Keep timing, withdraw delta reference when its trace cannot fit.
                self.trace = []
                self.trace_overflow = True
            self._delta(obs)
            self.lap_reason = None
        self.previous_gps = obs

    def _delta(self, obs):
        self.lap_delta = None
        candidates = []
        for (a, ta), (b, tb) in zip(self.best_trace, self.best_trace[1:]):
            x, y = xy(obs.value, a)
            bx, by = xy(b, a)
            square = bx * bx + by * by
            if square <= 0:
                continue
            fraction = max(0, min(1, (x * bx + y * by) / square))
            error = math.hypot(x - bx * fraction, y - by * fraction)
            reference = ta + (tb - ta) * fraction
            # Bound reference progress across closed loops/self-intersections.
            # A nearby finish segment must not masquerade as the start segment.
            floor = max(0, (self.reference_progress or 0) - .5)
            ceiling = (self.reference_progress or 0) + 3
            if floor <= reference <= ceiling:
                candidates.append((error, reference))
        if candidates:
            error, reference = min(candidates)
            if error <= 10 + obs.value[2]:
                self.lap_delta = (obs.acquired_ns - self.lap_started) / NS - reference
                self.reference_progress = max(self.reference_progress or 0, reference)

    def reset_trip(self, now):
        self.advance(now)
        self.trip.reset(now)

    def reset_session(self, now):
        self.advance(now)
        self._abort_speed('Session ended')
        self._abort_lap('Session ended')
        self._event('session-ended', startNs=self.session_started, endNs=now,
                    peaks=copy.deepcopy(self.peaks), peakObservations=copy.deepcopy(self.peak_observations),
                    origins=sorted(self.peak_sources))
        self.session_id += 1
        self.session_started = now
        self.peaks = dict.fromkeys(self.peaks)
        self.peak_sources.clear()
        self.peak_observations.clear()
        self._reset_active()
        self.acceleration_summary.reset(now)
        self.rolling100_to_200.reset_session(now)

    def model(self, now):
        self.advance(now)
        acceleration = self.inputs.get('acceleration')
        available = acceleration is not None and acceleration.fresh(now)
        return {'version': 1, 'configured': True, 'sessionId': self.session_id,
                'sourceGeneration': self.generation, 'observedNs': now,
                'inputs': {key: self.inputs.get(key, Observation(None, None, availability='unavailable')).model(now)
                           for key in ('speed_kmh', 'acceleration', 'gps_position')},
                'gMeter': {'longitudinalG': acceleration.value[0] / G if available else None,
                           'lateralG': acceleration.value[1] / G if available else None,
                           'peaks': self.peaks.copy(), 'peakObservations': copy.deepcopy(self.peak_observations),
                           'peakOrigins': sorted(self.peak_sources),
                           'summary': self.acceleration_summary.model(self.session_id, self.generation,
                                                                      acceleration, now)},
                'trip': self.trip.model(bool(self.inputs.get('speed_kmh') and
                                               self.inputs['speed_kmh'].fresh(now))),
                'rolling100To200': self.rolling100_to_200.model(),
                'timers': copy.deepcopy(self.timers),
                'laps': {'line': copy.deepcopy(self.line),
                         'direction': 'negative-to-positive cross product A→B',
                         'phase': 'running' if self.lap_started is not None else 'waiting',
                         'elapsedSeconds': ((self.previous_gps.acquired_ns - self.lap_started) / NS
                                            if self.lap_started is not None and self.previous_gps else None),
                         'last': copy.deepcopy(self.last_lap), 'best': copy.deepcopy(self.best_lap),
                         'deltaSeconds': self.lap_delta, 'reason': self.lap_reason},
                'history': copy.deepcopy(list(self.history)), 'revision': self.revision}
