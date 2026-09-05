# Control API

[`stage_control/run.py`](../stage_control/run.py) is the only program that opens
a serial port. All other programs use its Flask API on port **5000**. This
includes the notebooks, the calibration scripts, and manual `curl` commands.

The system has three layers:

```text
command_center/api_client.py   Python client. One method for each endpoint.
        HTTP :5000             Flask routes in run.py
   stage_control.py            Serial protocols for the ESP and the Pico devices
        USB serial             2 stage controllers and 4 Pico devices
```

## HTTP endpoints

All the endpoints use the GET method. The two upload endpoints use the POST
method. The reply is the text `Done.` if the table does not give a different
reply.

### Stage movement

| Route | Reply | Function |
|---|---|---|
| `/` | `Server up.` | Tells you that the server operates. |
| `/move/<ax>/<loc>` | | Moves the axis `ax` to the absolute position `loc` in millimeters. Stops the axis first. Returns immediately. The movement continues. |
| `/velocity/<ax>/<speed>` | | Sets the velocity of the axis. The subsequent movements use this velocity. |
| `/is_moving` | `{"is_moving": bool}` | Tells you if one or more axes move. |
| `/get_is_moving` | `{1: bool, 2: bool, 3: bool}` | Tells you which axes move. |
| `/get_positions` | `{1: float, 2: float, 3: float}` | Gives the position of each axis in millimeters. |
| `/emergency_stop` | | Sends the abort command to each controller. |

The axis numbers are **1 = z, 2 = x, 3 = y**. These are not the numbers of the
physical connections. `run.py` connects the logical axes to two Newport
controllers:

| Logical axis | Name | Controller | Physical axis |
|---:|---|---|---:|
| 1 | z | `stages[1]` | 1 |
| 2 | x | `stages[0]` | 1 |
| 3 | y | `stages[0]` | 2 |

The class `api_client.APIClient` uses keyword arguments for the axes. Use this
class:

```python
a = api_client.APIClient("http://localhost:5000")
a.velocity(x=1, y=1)
a.move(x=-15.0, y=21.1)
a.wait_for_move()          # Reads /is_moving each 50 ms
print(a.location())        # {'1': z, '2': x, '3': y}
```

### Triggers

| Route | Function |
|---|---|
| `/trigger` | Sends the default trigger. This is one group of frames on all the channels. |
| `/trigger/<channel>/<frames>/<stage>/<notify>` | Sends a trigger with all the parameters. |

The parameters are:

- **`channel`** is `A` for all the channels, or `0`, `1`, or `2`. The server
  reads all values that start with `A` as "all channels". The server reads all
  other values as an integer.
- **`frames`** is the number of frames in the group.
- **`stage`** is `Y` to move the stage between the frames. It is `N` to keep the
  stage at the same position.
- **`notify`** is `Y` if the Pico must report the end of the operation. It is
  `N` if the Pico must not report.

The server reads only the first character of `stage` and `notify`. Thus `Yes`
and `No` are also correct values.

**This endpoint does not return until the Pico reports the end of the
operation.** A group of 2000 frames keeps the Flask thread busy for the full
capture time.

### Galvo waveforms and AOTF waveforms

| Route | Method | Body | Function |
|---|---|---|---|
| `/reset_galvo/<id>` | GET | | Sets the position in the DAC table to the first value. |
| `/upload_wavetable/<id>` | POST | multipart `file` | Sends the galvo positions as hexadecimal values with commas between them. |
| `/upload_aotf/<id>` | POST | multipart `file` | Sends the laser control data as one `Y` byte or `N` byte for each line. |

The value `<id>` is the **channel number 0, 1, or 2**. `run.py` connects each
channel number to one laser:

| Channel number | Laser | Default table |
|---:|---:|---|
| 0 | 488 nm | [`488.txt`](../stage_control/488.txt) |
| 1 | 560 nm | [`560.txt`](../stage_control/560.txt) |
| 2 | 642 nm | [`642.txt`](../stage_control/642.txt) |

Each table must have **2554 values**. The table has 150 lead-in lines, 2304
active lines, and 100 trailing lines. The system uses one value for each sensor
line. See [architecture.md](architecture.md#the-three-time-scales).

The formats are:

```text
/upload_wavetable   0x2a01,0x2a01,0x2a02,...     uint16 DAC values in hexadecimal
/upload_aotf        NNNN...YYYY...NNNN           one byte for each line, Y = laser on
```

`APIClient` makes the two formats from Python lists:

```python
a.apply_dac_wavetable(0, [0x2a01] * 2554)                        # integers
a.apply_aotf_table(0, ([0] * 150) + ([1] * 2304) + ([0] * 100))  # 1 = laser on
a.reset_galvo(0)
```

The new table becomes active immediately. But the position in the table does not
change. If the next group of frames must start at the first value, send
`/reset_galvo/<id>` after the upload. All the experiment scripts in this
repository do the upload first and the reset second.

### Gamepad

| Route | Function |
|---|---|
| `/disable_gamepad` | Ignores all the gamepad events. |
| `/enable_gamepad` | Uses the gamepad events again. |

Disable the gamepad before a long automatic scan. This prevents a movement of
the stage from a gamepad stick that is not at the center position.

## Serial protocols

The file
[`stage_control/stage_control.py`](../stage_control/stage_control.py) contains
the serial protocols. Each device has its own `threading.Lock`. Thus the Flask
thread and the gamepad thread cannot mix their bytes on the serial line.

### Newport ESP stage controllers at 19200 baud

The class `ESPStageControl` uses the standard ESP commands. Each command ends
with a newline character.

| Command | Method | Function |
|---|---|---|
| `<ax>OR` | `home` | Moves one axis to the home position. |
| `OR` | `home_all` | Moves all the axes to the home position. |
| `TP` | `get_current_position` | Gives the position. The reply has float values with commas between them. |
| `TS` | `get_is_moving` | Gives the status. See the bit format below. |
| `<ax>ST` | `stop` | Decreases the velocity to zero. |
| `AB` | `emergency_stop` | Stops the movement immediately. |
| `<ax>PA<pos>` | `send_move` | Moves to an absolute position. The command `WT50` goes first. |
| `<ax>VA<vel>` | `send_velocity` | Sets the velocity. |
| `<ax>MV<+ or ->` | `send_move_indefinite` | Moves until a stop command. The gamepad uses this command. |
| `<ax>MO` | `send_enable_axis` | Energizes the motor. |

The method `get_is_moving` reads the first byte of the `TS` reply. Then it
changes the byte to a binary text and reverses the sequence. The three lowest
bits give the status of the axes 1, 2, and 3:

```python
rv = bin(rv[0])[2:][::-1][:3]
return {i + 1: (v == "1") for i, v in enumerate(rv)}
```

The method `send_move` sends the command `WT50` first. This makes a delay of
50 ms. It also sends an `ST` command first. Thus the controller stops a movement
that is in operation before it starts the new movement.

### Pico trigger controller at 115200 baud

The method `TriggerControl.send_trigger` writes one ASCII command:

```text
T <channel> <stage> <notify> <frames> \r

  T          The first character of the command
  channel    'A' for all the channels, or the channel number
  stage      'Y' or 'N'. Moves the stage between the frames.
  notify     'Y' or 'N'. Reports the end of the operation on the serial line.
  frames     The number of frames
  \r         The last character of the command
```

This example sends 1000 frames on all the channels. The stage moves and the Pico
reports the end of the operation:

```text
TAYY1000\r
```

The Pico sends one line as the reply. The character `D` tells you that the
operation is complete. The method `is_done()` reads the serial line until it
gets a line with data. This is why the `/trigger/...` endpoint does not return
immediately.

### Pico DAC and AOTF controllers at 115200 baud

There is one of these controllers for each laser. The class `DACControl` writes
one key byte and then the table. The table has little-endian values.

| Key | Format | Function |
|---|---|---|
| `S` | 2 bytes for each value | Sends the galvo DAC wavetable. |
| `A` | 1 byte for each value | Sends the AOTF table. |
| `R` | No data | Sets the position in the table to the first value. |

The Pico replies with `D` after each command. This is the same reply as the
trigger controller.

The method `load_defaults()` operates at the start of `run.py` for the three
lasers. It resets the Pico. Then it reads the DAC table from the `.txt` file of
that channel. Then it sends the default AOTF table. The default table is
150 values of "off", 2304 values of "on", and 100 values of "off". Thus the
laser is on for the active sensor lines only.

### Table files

The files [`488.txt`](../stage_control/488.txt),
[`560.txt`](../stage_control/560.txt), and
[`642.txt`](../stage_control/642.txt) each contain one line. The line has 2554
uint16 DAC values in hexadecimal with commas between them. These are the
calibrated galvo movements for that laser. The calibration procedure in
[calibration.md](calibration.md) makes these files. The files keep the
calibration data after a restart of `run.py`.

Do not change these files manually. Use the calibration procedure to make new
files. One incorrect value gives a bright line at that sensor line in each
frame.

## Gamepad functions

The main thread of `run.py` reads an Xbox 360 gamepad. It uses the file
[`Gamepad.py`](../stage_control/Gamepad.py) from
[piborg/Gamepad](https://github.com/piborg/Gamepad). The gamepad moves the
stages.

| Control | Function |
|---|---|
| Right stick X and Y | Moves the x axis and the y axis. The velocity increases with the movement of the stick. The y axis is inverted. |
| Left stick X and Y | Moves the same axes at 4 % of the velocity for accurate positions. |
| D-pad up and down | Moves the z axis at `z_velocity_max`. |
| D-pad left and right | Moves the z axis at 10 % of `z_velocity_max`. |
| `A` | Sends a trigger for one frame on all the channels. |
| `B` | Decreases the velocity scale in steps of 20 %. At 0 % the value changes to 100 %. |
| `X` | Stops the three axes. |
| `Y` | Energizes the stage motors again. |
| `LB` | Moves the z axis to the home position. |
| `RB` | Moves the x axis and the y axis to the home position. |

If the movement of a stick is less than `deadzone` (0.01), the program stops
that axis. It does not send a low velocity.
