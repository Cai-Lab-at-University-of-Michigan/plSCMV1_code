import serial
import threading
import time


class ESPStageControl:
    def __init__(self, serial_name: str, serial_baud: int, timeout: int):
        self._serial = serial.Serial(serial_name, serial_baud, timeout=timeout)
        self._serial_mutex = threading.Lock()

    def __del__(self):
        self.close()

    def __repr__(self):
        return (
            "ESPStageControl(connected on "
            + repr(self._serial)
            + ") -> "
            + repr(self.status())
        )

    def close(self) -> None:
        with self._serial_mutex:
            self._serial.close()

    def home(self, axis: int) -> None:
        cmd = f"{axis}OR\n"
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def home_all(self) -> None:
        with self._serial_mutex:
            self._serial.write(b"OR\n")

    def get_current_position(self) -> dict:
        with self._serial_mutex:
            self._serial.write(b"TP\n")
            rv = self._serial.readline().strip().split(b",")
            return {i + 1: float(x) for i, x in enumerate(rv)}

    def emergency_stop(self, axis: int) -> None:
        with self._serial_mutex:
            self._serial.write(b"AB\n")

    def stop(self, axis: int) -> None:
        cmd = f"{axis}ST\n"
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def get_is_moving(self) -> dict:
        with self._serial_mutex:
            self._serial.write(b"TS\n")
            rv = self._serial.readline().strip()  # get result from stage
            rv = bin(rv[0])[2:][::-1][:3]
            return {i + 1: (v == "1") for i, v in enumerate(rv)}  # check order

    def is_moving(self) -> bool:
        return any(self.get_is_moving().values())

    def wait_for_move(self) -> None:
        while self.is_moving():
            pass  # maybe replace with less busy wait

    def send_move(self, axis: int, position: float) -> None:
        cmd = f"WT50\n{axis}PA{position}\n"
        self.stop(axis)
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def send_velocity(self, axis: int, velocity: float) -> None:
        cmd = f"{axis}VA{velocity}\n"
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def send_move_indefinite(self, axis: int, dir: str) -> None:
        if not ((dir == "+") or (dir == "-")):
            return
        cmd = f"{axis}MV{dir}\n"
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def send_move_wait(self, axis: int, position: float) -> None:
        self.send_move(axis, position)
        self.wait_for_move()

    def send_enable_axis(self, axis: int) -> None:
        cmd = f"{axis}MO\n"
        with self._serial_mutex:
            self._serial.write(bytes(cmd, "utf-8"))

    def status(self) -> dict:
        return {
            "position": self.get_current_position(),
            "axes_moving": self.get_is_moving(),
            "stage_active_flag": self.is_moving(),
        }


class TriggerControl:
    def __init__(self, serial_name: str, serial_baud: int, timeout: int):
        self._serial = serial.Serial(serial_name, serial_baud, timeout=timeout)
        self._serial_mutex = threading.Lock()

    def is_done(self):
        while True:
            print('wait')
            rv = self._serial.readline().strip()
            if len(rv) > 0:
                print('done')
                return rv == b'D'

    def send_trigger(self, channel=None, frames=1000, stage=True, notify=False):
        print('here')
        payload = b"T"

        # Specify the channel
        if channel is None:
            payload += b"A"
        elif type(channel) is int:
            payload += bytes(str(channel), "utf-8")
        else:
            raise ValueError("Invalid channel identifier!")

        payload += b"Y" if stage else b"N"
        payload += b"Y" if notify else b"N"
        payload += bytes(str(frames), "utf-8")
        payload += b"\r"

        with self._serial_mutex:
            self._serial.write(payload)
            return self.is_done()


class DACControl:
    def __init__(
        self, serial_name: str, serial_baud: int, timeout: int, default_table=""
    ):
        self._serial = serial.Serial(serial_name, serial_baud, timeout=timeout)
        self._serial_mutex = threading.Lock()
        self.default_table = default_table
        self.DAC_table = []
        self.AOTF_table = []

    @staticmethod
    def load_table(raw):
        raw = raw.strip().split(b",")
        return [int(x, 16) for x in raw]

    def is_done(self):
        while True:
            rv = self._serial.readline().strip()
            if len(rv) > 0:
                return rv == b'D'

    def send_table(self, wavetable, key=b"S", byte_depth=2):
        payload = key + b"".join(
            map(lambda x: x.to_bytes(byte_depth, "little"), wavetable)
        )
        with self._serial_mutex:
            sent = self._serial.write(payload)
            print(f'Sent {sent} bytes')
            return self.is_done()
            
    def send_wavetable(self):
        return self.send_table(self.DAC_table, key=b"S", byte_depth=2)

    def send_AOTF_table(self):
        return self.send_table(self.AOTF_table, key=b"A", byte_depth=1)

    def reset(self):
        with self._serial_mutex:
            self._serial.write(b"R")
            done = self.is_done()

            return True

    def load_defaults(self):
        print("Performing reset...")
        self.reset()

        print(f"Loading DAC table from file... [{self.default_table}]")
        with open(self.default_table, "rb") as f:
            r = f.read()
            self.DAC_table = self.load_table(r)

        print("Applying DAC function...")
        start = time.time()
        rv = self.send_wavetable()
        print(f"\tFinished in {(time.time() - start)}s [{rv}]")

        print("Applying AOTF function...")
        start = time.time()
        self.AOTF_table = ([0] * 150) + ([1] * 2304) + ([0] * 100)
        rv = self.send_AOTF_table()
        print(f"\tFinished in {time.time() - start} [{rv}]")
