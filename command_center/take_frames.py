import time
import tqdm
import requests
import io

import api_client
import tifffile

a = api_client.APIClient("http://localhost:5000")

for channel in [0, 1, 2]:
    img_writer = tifffile.TiffWriter(f"test_{channel}.tif")

    aotf_table = ([0] * 150) + ([1] * 2304) + ([0] * 100)
    aotf_table = ([0] * 150) + ([1] * 1152) + ([0] * (1152)) + ([0] * 100)

    l = len(aotf_table)
    gv = [30000, 40000]
    wave_table = []
    for v in gv:
        wave_table += [v] * (l // len(gv))
    # a.apply_dac_wavetable(channel, wave_table)

    # a.reset_galvo(channel)

    for frame in tqdm.trange(144 // 2):
        aotf_table = [0] * 150
        for i in range(144 // 2):
            aotf_table += [1] * 1
            # aotf_table += [1 if i==frame else 0]*1
            aotf_table += [0] * (31)
        aotf_table += [0] * 100

        a.apply_aotf_table(channel, aotf_table)

        a.trigger_expanded(channel=channel, frames=1, stage=False, notify=True)

        camera_url = "http://localhost:5001/get_image"
        res = requests.get(camera_url)

        tif_buffer = io.BytesIO()
        tif_buffer.write(res.content)
        tif_buffer.seek(0)
        img = tifffile.imread(tif_buffer)

        img_writer.write(img)

        del res, img, tif_buffer

        time.sleep(0.1)

    img_writer.close()
