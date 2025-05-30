import sys

sys.path.append("../")

import time
import tqdm
import requests
import io

import urllib
import api_client
import tifffile


def set_string(in_str):
    url = f"http://10.156.2.28:8090/{in_str}"
    res = urllib.request.urlopen(url).read().decode()
    return res


a = api_client.APIClient("http://localhost:5000")

set_string(f"scan_2nd_{int(time.time())}")
time.sleep(1)


for channel in [0, 1, 2]:
    # img_writer = tifffile.TiffWriter(f"{channel}_{time.time()}.tif", bigtiff=True)
    for galvo_value in tqdm.tqdm(range(0, 2**16, 1024 // 4)):
        for pulse_width in [2304]:
            aotf_table = [0] * 150
            pulse = [0] * 2304
            for j in range(pulse_width):
                pulse[j] = 1
            aotf_table += pulse
            aotf_table += [0] * 100
            a.apply_aotf_table(channel, aotf_table)

            wave_table = [galvo_value] * len(aotf_table)
            a.apply_dac_wavetable(channel, wave_table)

            a.reset_galvo(channel)

            a.trigger_expanded(channel=channel, frames=1, stage=False, notify=True)

#            camera_url = "http://localhost:5001/get_image"
#            res = requests.get(camera_url)

#            tif_buffer = io.BytesIO()
#            tif_buffer.write(res.content)
#            tif_buffer.seek(0)
#            img = tifffile.imread(tif_buffer)

#            img_writer.write(img)

#            del res, img, tif_buffer

#    img_writer.close()
