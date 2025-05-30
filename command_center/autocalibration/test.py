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

set_string(f"line_scan_{time.time()}")
time.sleep(1)

from continuous_calibration import *

channel_map = {0: 0, 1: 2, 2: 1}

N = 1

from matplotlib import pyplot as plt

for i in range(1000):
    for channel in [0, 1, 2]:
        correction_test = {
            0: array(
                [
                    -5.22577467e-29,
                    5.83468555e-25,
                    -2.73871155e-21,
                    7.00966680e-18,
                    -1.06068688e-14,
                    9.60634981e-12,
                    -4.99747392e-09,
                    1.30783698e-06,
                    -9.93671629e-05,
                    -1.59895516e-02,
                    6.35086901e00,
                ]
            ),
            1: array(
                [
                    3.61910246e-29,
                    -5.46352715e-25,
                    3.45028900e-21,
                    -1.19682405e-17,
                    2.50638198e-14,
                    -3.26869387e-11,
                    2.63393514e-08,
                    -1.25332626e-05,
                    3.19246595e-03,
                    -3.43745221e-01,
                    -4.73066339e00,
                ]
            ),
            2: array(
                [
                    -2.69339599e-29,
                    2.94664934e-25,
                    -1.35311080e-21,
                    3.37899801e-18,
                    -4.96058518e-15,
                    4.29477472e-12,
                    -2.02970221e-09,
                    3.58694978e-07,
                    8.05047035e-05,
                    -4.26918582e-02,
                    6.99404206e00,
                ]
            ),
        }

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

        if i == 0:
            a.apply_dac_wavetable(channel, wt)
            a.apply_aotf_table(channel, ([0] * 150) + ([1] * 2304) + ([0] * 100))
            a.reset_galvo(channel)

        a.trigger_expanded(channel=channel, frames=N, stage=False, notify=True)
        
        time.sleep(.5)
