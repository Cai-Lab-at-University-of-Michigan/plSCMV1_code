import sys

sys.path.append("/home/loganaw/cameraman/command_center")

import time
import tqdm
import requests
import io

import urllib
import api_client
import tifffile

import numpy as np
from numpy import array


def set_string(in_str):
    url = f"http://10.156.2.28:8090/{in_str}"
    res = urllib.request.urlopen(url).read().decode()
    return res


a = api_client.APIClient("http://localhost:5000")

set_string(f"line_scan_{int(time.time())}")
time.sleep(1)

from continuous_calibration import *

channel_map = {0: 0, 1: 2, 2: 1}

N = 100

from matplotlib import pyplot as plt

for _ in range(1):
    for channel in [0, 1, 2]:
        correction_test = {0: array([ 3.73212399e-30, -4.69891869e-26,  2.58843347e-22, -8.12279970e-19,
         1.58321529e-15, -1.96093350e-12,  1.51860394e-09, -6.96725472e-07,
         1.70168791e-04, -1.78011831e-02,  2.04031691e-01]),
 1: array([-1.28736653e-28,  1.44153132e-24, -6.82309114e-21,  1.77782496e-17,
        -2.78381873e-14,  2.68739513e-11, -1.58308711e-08,  5.47177894e-06,
        -1.03005531e-03,  8.28313372e-02,  3.33969277e+00]),
 2: array([ 9.05016552e-30, -1.12305115e-25,  5.96962476e-22, -1.77396475e-18,
         3.22382363e-15, -3.67753890e-12,  2.59278964e-09, -1.06036088e-06,
         2.14478933e-04, -1.27329473e-02, -9.95703084e-02])}

        correction_2d = lambda x: np.polyval(correction_test[channel_map[channel]], x)
        #correction_2d = None

        wt = calculate_calibrated_galvo(
            channel_map[channel],
            second_order_correction=correction_2d,
            second_scale=(-1 if channel == 2 else 1),
        )

        if False:  # True:
            plt.plot(wt)
            plt.show()

        a.apply_dac_wavetable(channel, wt)

        a.apply_aotf_table(channel, ([0] * 150) + ([1] * 2304) + ([0] * 100))

        a.reset_galvo(channel)

        a.trigger_expanded(channel=channel, frames=N, stage=False, notify=True)
