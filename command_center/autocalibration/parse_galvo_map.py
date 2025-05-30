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
