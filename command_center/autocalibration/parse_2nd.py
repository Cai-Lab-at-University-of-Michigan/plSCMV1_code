"""Calculates the line data from the archives of the second-order scan.

This script is the same as ``parse_galvo_map.py``. But it calculates the mean
along ``axis=0`` and not ``axis=1``, because this scan changes in the other
direction. It writes the result to ``save.p``.

This is step 2 of 3 in the procedure for the second-order correction. See
docs/calibration.md.
"""

import zipfile
import glob
import zstd
import numpy as np
import tqdm
import pickle

res = {}

files = sorted(list(filter(lambda x: ("DEFAULT" not in x), glob.glob("*.zip"))))
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
                d = d.mean(axis=0).ravel()
                res[f][ff] = d


pickle.dump(res, open("save.p", "wb"))
