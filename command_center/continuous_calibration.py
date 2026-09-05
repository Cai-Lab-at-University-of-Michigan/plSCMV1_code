"""Calibration library for the galvo wavetables and the signal alignment.

All the scripts in ``autocalibration/`` use this module. The module has three
groups of functions.

**Wavetable synthesis.** The function :func:`calculate_calibrated_galvo` applies
the position map, the second-order correction, and the phase offset. It gives
the uint16 table of 2554 values for a DAC Pico. The value 2554 is the sum of
150 lead-in lines, 2304 sensor lines, and 100 trailing lines. The system uses
one value for each line of the rolling shutter.

**Fit functions.** The class :class:`poly_from_fit` contains ``np.polyfit`` and
adds the method :meth:`~poly_from_fit.invert`. That method calculates the
inverse directly for a degree of 0, 1, or 2. This is why the fits from a DAC
value to a line number have a degree of 2. The fit in the other direction has a
degree of 3.

**Signal alignment.** The function :func:`calculate_signal_offset` measures the
difference in phase between the line data of two channels. It uses a moving
window and more samples to measure an offset of less than one line. The function
:func:`map_calibration` changes that result to line coordinates.

The system has two different sets of channel numbers. The variable
``channel_maps`` gives the calibration index for each API channel as
``[(0, 0), (1, 2), (2, 1)]``. Give the calibration index to
:func:`calculate_calibrated_galvo`. Give the API channel to
``APIClient.apply_dac_wavetable``.

The files ``fit_calibration.p`` and ``fit_hamamatsu.p`` contain
:class:`poly_from_fit` objects in the pickle format. This module must be
available with this name to read those files.

For the full procedure, see docs/calibration.md.
"""

import pickle
import numpy as np
import tifffile
from scipy.interpolate import CubicSpline
import scipy.signal
import requests
import io

aotf_default = ([0] * 150) + ([1] * 2304) + ([0] * 100)
aotf_strobe = (
    ([0] * 150) + [(1 if (i % 16 == 0) else 0) for i in range(2304)] + ([0] * 100)
)


class camera_api_client:
    def __init__(self, url):
        self.url = url

    def __len__(self):
        res = requests.get(self.url + "buffer_size").content
        return int(res)

    def clear_buffer(self):
        res = requests.get(self.url + "clear_buffer")
        return res.content

    def get_image(self):
        res = requests.get(self.url + "get_image").content
        tif_buffer = io.BytesIO()
        tif_buffer.write(res)
        tif_buffer.seek(0)
        return tifffile.imread(tif_buffer)


def flatten_array(sig):
    return np.array(sig).astype(float).mean(axis=0)


def tif_to_signal(fname):
    with tifffile.TiffFile(fname) as tif:
        out = [flatten_array(p.asarray()) for p in tif.pages]
        return np.array(out).mean(axis=0)  # average together


def calculate_signal_offset(
    s1_in, s2_in, scale=100, window_size=100, step_size=25, phase_size=3
):
    out = []
    idxs = list(range(0, len(s1_in), step_size))
    if (len(s1_in) - 1) not in idxs:
        idxs.append(len(s1_in) - 1)

    for window_start in idxs:
        s1 = s1_in[window_start:][:window_size]
        s2 = s2_in[window_start:][:window_size]

        s1 = scipy.signal.resample(s1, num=len(s1) * scale).astype(float)
        s2 = scipy.signal.resample(s2, num=len(s2) * scale).astype(float)

        s1 /= s1.max()
        s2 /= s2.max()

        phase_vals = range(-phase_size * scale, (phase_size * scale) + 1)
        max_phase = []

        for i in phase_vals:
            s2_rolled = np.roll(s2, i)
            toadd = s1 - s2_rolled
            toadd = np.abs(toadd)
            toadd = toadd.mean()
            max_phase.append(toadd)

        max_phase = np.argmin(max_phase)
        max_phase = phase_vals[max_phase]

        out.append(max_phase / float(scale))

    cs = CubicSpline(idxs[: len(out)], out)
    return cs, cs([i for i, _ in enumerate(s1_in)])


class poly_from_fit:
    def __init__(self, fit_in):
        self.fit = fit_in

    @property
    def d(self):
        return len(self.fit) - 1

    @staticmethod
    def fit(x, y, d):
        fit = np.polyfit(x, y, d)
        return poly_from_fit(fit)

    def invert(self, y):
        if type(y) is list:
            y = np.array(y)
        if self.d == 0:
            return self(0)  # constant
        elif self.d == 1:
            return (y - self.fit[1]) / self.fit[0]
        elif self.d == 2:
            a, b, c = self.fit
            return np.stack(
                [
                    (-b + (i) * np.sqrt((b**2) - (4.0 * a * c) + (4 * a * y)))
                    / (2.0 * a)
                    for i in [1, -1]
                ]
            )
        else:
            raise NotImplementedError("Must be degree <= 2")

    def __call__(self, x):
        if type(x) is list:
            x = np.array(x)
        return sum(a * ((x) ** i) for i, a in enumerate(reversed(self.fit)))

    def __repr__(self):
        return f"<polynomial_from_fit degree=({self.d}) fit=({self.fit})>"


def load_pickle(fname):
    return pickle.load(open(fname, "rb"))


def get_calibrations():
    fit_calibration = load_pickle("fit_calibration.p")
    fit_hamamatsu = load_pickle("fit_hamamatsu.p")

    return fit_calibration, fit_hamamatsu


channel_maps = [(0, 0), (1, 2), (2, 1)]  # cal, ham


def calculate_calibrated_galvo(
    channel,
    second_order_correction=None,
    time_offset=None,
    offset_phase=36,
    extend_by=50,
    sensor_size=2304,
    second_scale=1.0,
    time_scale=1.0,
):
    dac_possible_values = np.arange(-100, sensor_size + 100, dtype=float)

    if second_order_correction is not None:
        correction_fit_results = [
            second_order_correction(i) for i in dac_possible_values
        ]
        correction_fit_results = np.array(correction_fit_results, dtype=float)
        correction_fit_results *= np.tan(13.6 / 180 * np.pi)
        correction_fit_results *= second_scale

        dac_possible_values -= correction_fit_results

    # TODO add time offset routine
    if time_offset is not None:
        dac_possible_values += time_offset * time_scale

    # Calculate a mapped galvo profile
    try:
        fit_hamamatsu = pickle.load(open("ham_cal.p", "rb"))
    except Exception:
        fit_hamamatsu = pickle.load(open("autocalibration/ham_cal.p", "rb"))

    for_dac = [np.polyval(fit_hamamatsu[channel], i) for i in dac_possible_values]
    for_dac = np.array(for_dac).astype(float)

    # TODO add smoothing

    # Phase offset
    new_for_dac = np.zeros(shape=for_dac.shape[0] + extend_by, dtype=float)
    new_for_dac += for_dac[0]
    new_for_dac[
        extend_by - offset_phase : extend_by - offset_phase + len(for_dac)
    ] = for_dac[:]
    for_dac = new_for_dac.astype(np.uint16)

    return for_dac


def map_calibration(cs_offset, cal_fit, ham_fit, size=2304, zero=0):
    out = []
    for x in range(-100, size + 100):
        x_map = ham_fit.invert(x)[zero]
        x_map_cal = cal_fit(x_map)
        cs_map = cs_offset(x_map_cal)
        out.append(cs_map)
    return np.array(out, dtype=float)
