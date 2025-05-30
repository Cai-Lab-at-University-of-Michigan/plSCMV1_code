import urllib.request
import json
import time
import io
import requests

axis_map = {"z": 1, "x": 2, "y": 3}


class APIClient:
    def __init__(self, host):
        self.ip = host

    def __repr__(self):
        return "APIClient(connected on " + self.ip + ")"

    def move(self, x=None, y=None, z=None):
        if x != None:
            url = f'{self.ip}/move/{axis_map["x"]}/{x}'
            res = urllib.request.urlopen(url).read().decode()
        if y != None:
            url = f'{self.ip}/move/{axis_map["y"]}/{y}'
            res = urllib.request.urlopen(url).read().decode()
        if z != None:
            url = f'{self.ip}/move/{axis_map["z"]}/{z}'
            res = urllib.request.urlopen(url).read().decode()

    def velocity(self, x=None, y=None, z=None):
        if x != None:
            url = f'{self.ip}/velocity/{axis_map["x"]}/{x}'
            res = urllib.request.urlopen(url).read().decode()
        if y != None:
            url = f'{self.ip}/velocity/{axis_map["y"]}/{y}'
            res = urllib.request.urlopen(url).read().decode()
        if z != None:
            url = f'{self.ip}/velocity/{axis_map["z"]}/{z}'
            res = urllib.request.urlopen(url).read().decode()

    def location(self):
        url = f"{self.ip}/get_positions"
        res = urllib.request.urlopen(url).read().decode()
        parsed = json.loads(res)
        return parsed

    def trigger(self):
        url = f"{self.ip}/trigger"
        res = urllib.request.urlopen(url).read().decode()
        return res

    def trigger_expanded(self, channel, frames, stage, notify):
        url = f"{self.ip}/trigger/{channel}/{frames}/"
        url += "Y" if stage else "N"
        url += "/"
        url += "Y" if notify else "N"
        res = urllib.request.urlopen(url).read().decode()
        return res

    def is_moving(self) -> bool:
        url = f"{self.ip}/is_moving"
        res = urllib.request.urlopen(url).read().decode()
        return json.loads(res)["is_moving"]

    def wait_for_move(self) -> None:
        while self.is_moving():
            time.sleep(0.05)

    def reset_galvo(self, id):
        url = f"{self.ip}/reset_galvo/{id}"
        res = urllib.request.urlopen(url).read().decode()
        return res

    def apply_dac_wavetable(self, id, table):
        payload = io.BytesIO()
        table_formatted = ",".join(map(hex, table))
        payload.write(bytes(table_formatted, "utf-8"))
        payload.seek(0)

        rv = requests.post(f"{self.ip}/upload_wavetable/{id}", files={"file": payload})

        del payload
        del table_formatted

    def apply_aotf_table(self, id, table):
        payload = io.BytesIO()
        table_formatted = b"".join((b"Y" if x else b"N") for x in table)
        payload.write(table_formatted)
        payload.seek(0)

        rv = requests.post(f"{self.ip}/upload_aotf/{id}", files={"file": payload})

        del payload
        del table_formatted
