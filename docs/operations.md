# Operations

This document tells you how to start the plSCM system and how to do a scan.

## Prerequisites

### Control PC (Linux)

Install the Python packages:

```bash
pip install pyserial flask tqdm requests tifffile numpy scipy zstd matplotlib
```

The user that starts `run.py` must have permission to read and write the serial
devices. Usually the user must be a member of the group `dialout` or the app must
be run with escalated privileges.

The program uses permanent device paths and not the paths `/dev/ttyUSB*`. Thus
you can disconnect a device and connect it again safely. The paths are:

- The stage controllers use the USB topology:
  `/dev/serial/by-path/pci-...-port0`
- The Pico devices use the serial number:
  `/dev/serial/by-id/usb-Raspberry_Pi_Pico_<SN>-if00`

These paths are at the start of [`run.py`](../stage_control/run.py). If you
replace a Pico, put its new serial number in `DAC_devs` or in `controller_dev`.
To find the serial number, use this command:

```bash
ls -l /dev/serial/by-id/
```

### Camera PC (Windows)

You must have these items:

- Visual Studio with the C++ desktop workload
- The Hamamatsu **DCAM-API** runtime and SDK. The file `dcamapi.lib` is in this
  repository. The headers and the runtime are not in this repository.
- The **zstd** library from vcpkg: `vcpkg install zstd:x64-windows`

Open the file
[`cameraman_windows.sln`](../cameraman_windows_stateless/cameraman_windows.sln).
Then build the x64 Release configuration. The application uses `Ws2_32`,
`Mswsock`, `AdvApi32`, `urlmon`, and `wininet`. These libraries are part of the
Windows SDK.

### Storage host

Build the server:

```bash
cd sndif_server && cargo build --release
```

The server needs Rust with the 2021 edition. An environment variable sets each
value in its configuration. See
[Configuration of the two Rust programs](#configuration-of-the-two-rust-programs).

## Start-up procedure

Do the steps in this sequence. The camera application makes a new connection for
each frame. The experiment scripts need the two servers.

**1. Start the receiver on the storage host.**

```bash
cd /data/plscm && /path/to/sndif_server
```

The server opens the files `DEFAULT_ch0.zip`, `DEFAULT_ch1.zip`, and
`DEFAULT_ch2.zip`. Then it waits. All the frames from before the first archive
name go to these three files.

**2. Start the device server on the control PC.**

```bash
cd stage_control && python run.py
```

At the start, the program resets the three DAC and AOTF Pico devices. Then it
reads the files `488.txt`, `560.txt`, and `642.txt`. Then it sends the default
AOTF table.

Look for three lines with the text `Finished in ...s [True]`. A value of `False`
tells you that the Pico did not reply. Then the illumination of that channel is
not correct.

The program then shows the text `Please connect your gamepad...`. It waits for a
gamepad at `/dev/input/js0`. **The Flask API operates at this time**, because it
uses a different thread. Thus you can use the system without a gamepad. This
message is not a failure message.

Do a test of the API:

```bash
curl http://localhost:5000/            # The reply is: Server up.
curl http://localhost:5000/get_positions
```

**3. Start the capture application on the camera PC.**

The application opens a console window for the log data. Then it opens the three
cameras and sets the timing parameters. Then it starts to capture. A window with
the name "Camera Live Preview" shows three panels of 576 pixels. Below the
panels the window shows the minimum value, the maximum value, and the frame
rate.

The application needs exactly three cameras (`CAMERA_NUMS`). If the number of
cameras is different, the function `init_api_and_cameras` stops the application.

**4. Move the stage to the home position** before the first scan. Use the button
`LB` for the z axis and the button `RB` for the x axis and the y axis. You can
also use the API.

## How to do a scan

All the experiment scripts use this sequence:

```python
import api_client, urllib.request, time

a = api_client.APIClient("http://localhost:5000")

def set_string(name):
    return urllib.request.urlopen(f"http://10.156.2.28:8090/{name}").read().decode()

a.velocity(x=1, y=1)
for x, y in positions:
    a.move(x=x, y=y)
    a.wait_for_move()

    set_string(f"coord_{x:+.3f},{y:+.3f}")   # Change to a new archive
    time.sleep(1)                            # Wait for the change

    a.trigger_expanded("A", 2000, True, True)
    time.sleep(10)                           # Wait for the last frames
```

Obey these four rules:

- **Send `set_string` before the trigger and not after it.** The name selects the
  archive for the frames that come next.
- **Wait after `set_string`.** The change of archive is not synchronous with the
  capture operation. The frames that are already in the queue go to the previous
  archive.
- **Wait after the group of triggers.** The method `trigger_expanded` returns
  when the Pico stops. It does not wait for the last frame to arrive at the
  storage host. If you set a new name too early, the last frames go to the next
  archive.

The notebook [`tile_scan.ipynb`](../command_center/tile_scan.ipynb) is an example
of this sequence. It makes a spiral of stage positions around a start position.
The distance between the positions is `STEP_SIZE`. Then it moves to each
position with the sequence above. The notebook sends `a.move(...)` two times for
each position. This makes sure that the stage receives the command.

To make sure that the system receives the data, look at the log of the server.
The server writes one line for each frame with a compression ratio. A ratio of
more than 10 tells you that the frames have almost no signal.

## Configuration of the two Rust programs

An environment variable sets each value in the configuration of `sndif_server`
and `sndif_test_client`. If a variable is not set, the program uses the default
value. Each program writes its configuration in the log at the start.

If a value is not correct, the program writes a message and stops with the exit
code 2. The server does this test before it opens the archive files. Thus an
incorrect configuration does not make files on the disk.

### sndif_server

| Variable | Default | Function |
|---|---|---|
| `SNDIF_FRAME_ADDRESS` | `0.0.0.0:8080` | The address for the frame input port |
| `SNDIF_CONTROL_ADDRESS` | `0.0.0.0:8090` | The address for the archive name port |
| `SNDIF_CAMERA_COUNT` | `3` | The number of cameras and thus the number of writer threads |
| `SNDIF_SERVER_THREADS` | `10` | The number of threads that parse the frames |
| `SNDIF_READ_BUFFER_SIZE` | `10485760` | The first size of the buffer for one frame in bytes |
| `SNDIF_OUTPUT_DIR` | `.` | The directory for the zip archives. The server makes this directory if it is not there. |
| `SNDIF_ARCHIVE_NAME` | `DEFAULT` | The name of the archive before the first name on the control port |

This example writes the archives to a different disk and uses different ports:

```bash
SNDIF_OUTPUT_DIR=/data/plscm/2026-09-04 \
SNDIF_FRAME_ADDRESS=0.0.0.0:9080 \
SNDIF_CONTROL_ADDRESS=0.0.0.0:9090 \
    sndif_server
```

### sndif_test_client

| Variable | Default | Function |
|---|---|---|
| `SNDIF_TEST_SERVER_ADDRESS` | `127.0.0.1:8080` | The address of the server |
| `SNDIF_TEST_CAMERA_COUNT` | `3` | The number of cameras |
| `SNDIF_TEST_THREADS_PER_CAMERA` | `2` | The number of threads for each camera |
| `SNDIF_TEST_FRAMES_PER_THREAD` | `500` | The number of frames from each thread |
| `SNDIF_TEST_FRAME_WIDTH` | `2304` | The width of a frame in pixels |
| `SNDIF_TEST_FRAME_HEIGHT` | `2304` | The height of a frame in pixels |
| `SNDIF_TEST_FRAME_BYTES_PER_PX` | `2` | The number of bytes for each pixel |

The default configuration sends 3000 frames of 10,616,832 bytes. This is
about 32 GB. Use a smaller frame size and a smaller number of frames for
a quick test:

```bash
SNDIF_TEST_FRAMES_PER_THREAD=10 \
SNDIF_TEST_FRAME_WIDTH=256 SNDIF_TEST_FRAME_HEIGHT=256 \
    sndif_test_client
```

The size of the header is not in this list. That value is 32 bytes and it is
part of the format on the line. See
[frame-protocol.md](frame-protocol.md#why-the-header-is-32-bytes).

## Shutdown procedure

**Use Ctrl-C to stop the server.** This is the correct method. The signal
handler stops the listener thread. Then the frame channels disconnect. Then each
writer thread calls `zip.finish()` and writes the central directory of the zip
file.

To stop the camera application, close the preview window. Then stop the process.