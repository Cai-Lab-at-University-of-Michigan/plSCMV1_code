"""Calculates the line data from the archives of the galvo position map.

This script reads each file ``*.zip`` in the work directory. It ignores all the
files with ``DEFAULT`` in the name. It decompresses each frame. Then it
calculates the mean along ``axis=1`` to get one line profile. It writes the
result to ``save.p`` in this format:
``{zip name: {member name: 1-D array}}``.

The archives with ``DEFAULT`` in the name contain the frames from the time
before the first archive name. See
docs/frame-protocol.md#how-to-set-the-archive-name-on-http-port-8090.

This is step 2 of 3 in the procedure for the position map. The notebook
``parse_galvo_map_2.ipynb`` calculates the fits. Note the axis: this script uses
``axis=1``, but ``parse_2nd.py`` uses ``axis=0``.
"""

import zipfile
import glob
import zstd
import numpy as np
import tqdm
import pickle

res = {}

files = sorted(list(filter(lambda x: "DEFAULT" not in x, glob.glob("*.zip"))))
print(files)

for i, f in enumerate(files):
    print(i, f)
    res[f] = {}
    with zipfile.ZipFile(f, mode="r") as zf:
        for ff in tqdm.tqdm(zf.namelist()):
            with zf.open(ff) as d:
                d = d.read()
                d = zstd.ZSTD_uncompress(d)
                d = np.frombuffer(d, dtype=np.uint16).reshape(2304, 2304)
                d = d.mean(axis=1)
                res[f][ff] = d


pickle.dump(res, open("save.p", "wb"))
