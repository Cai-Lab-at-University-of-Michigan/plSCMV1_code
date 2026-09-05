# Calibration

The microscope operates correctly only if three subsystems agree on the position
of each sensor line. The three subsystems are the galvo, the laser control
(AOTF), and the rolling shutter of the camera. Calibration makes the DAC
wavetable of 2554 values that gives this agreement. There is one wavetable for
each laser.

The library code is in
[`continuous_calibration.py`](../command_center/continuous_calibration.py). The
scripts that record the data and calculate the fits are in
[`autocalibration/`](../command_center/autocalibration/).

## The three corrections

The procedure corrects three different errors in this sequence:

1. **Position.** This correction gives the galvo DAC value that illuminates
   sensor line *n*. The system uses one polynomial fit for each channel. The
   file `ham_cal.p` contains these fits.
2. **Second-order distortion.** This correction removes the curve that stays
   after the linear fit. The system multiplies the correction by the tangent of
   the optical angle of 13.6 degrees. The file
   `second_order_results.py` contains one polynomial of 11 terms for each
   channel.
3. **Phase.** This correction moves the waveform forward by `offset_phase`
   (36) lines. If you give a time offset, the procedure also applies a different
   correction to each line. The system measures this offset from the line data
   of two channels.

## How to make a wavetable

The function `calculate_calibrated_galvo` applies the three corrections. It
gives the array that you send to the DAC:

```python
from continuous_calibration import calculate_calibrated_galvo
import numpy as np

wt = calculate_calibrated_galvo(
    channel,                                   # Calibration index, not API channel
    second_order_correction=lambda x: np.polyval(coeffs[channel], x),
    second_scale=-1 if channel == 2 else 1,    # The sign changes with the galvo direction
)
a.apply_dac_wavetable(api_channel, wt)
a.reset_galvo(api_channel)
```

The function does these steps:

| Step | Operation |
|---|---|
| 1 | Makes the line numbers `arange(-100, 2404)`. This gives 2504 lines. There are 100 more lines on each side of the sensor. |
| 2 | If you give a second-order correction, calculates the correction for each line. Multiplies the result by `tan(13.6 deg)` and by `second_scale`. Then **subtracts** the result from the line numbers. |
| 3 | If you give a time offset, multiplies it by `time_scale` and adds it. |
| 4 | Changes each corrected line number to a DAC value. Uses the cubic polynomial in `ham_cal.p` for this channel. |
| 5 | Increases the length of the array by `extend_by` (50). Puts the DAC values at the offset `extend_by` minus `offset_phase`. This offset is 14. Thus the galvo movement starts 36 lines before the shutter. |
| 6 | Changes the values to `uint16`. |

The result has **2504 plus 50 = 2554 values**. This is the length that the DAC
firmware needs. It is the same length as the line budget in
[architecture.md](architecture.md#the-three-time-scales).

## Channel numbers

The system has two different sets of channel numbers. They are not the same. If
you use the incorrect set, the galvo movement is not correct.

| API channel (`run.py` and `APIClient`) | Laser | Calibration index (`ham_cal.p` and the fits) |
|---:|---:|---:|
| 0 | 488 nm | 0 |
| 1 | 560 nm | **2** |
| 2 | 642 nm | **1** |

The scripts in `autocalibration/` use this dictionary:

```python
channel_map = {0: 0, 1: 2, 2: 1}   # API channel to calibration index
```

The variable `continuous_calibration.channel_maps` contains the same data as
`[(0, 0), (1, 2), (2, 1)]`.

Always give the **calibration index** to `calculate_calibrated_galvo`. Always
give the **API channel** to `apply_dac_wavetable`. The scripts do this:

```python
wt = calculate_calibrated_galvo(channel_map[channel], ...)   # Calibration index
a.apply_dac_wavetable(channel, wt)                           # API channel
```

## Calibration data files

| File | Format | Contents |
|---|---|---|
| [`autocalibration/ham_cal.p`](../command_center/autocalibration/ham_cal.p) | `{0,1,2: [4 floats]}` | A cubic polynomial that changes a line number to a DAC value. The procedure uses this file to make the wavetables. Channel 0 has a positive slope of about 15 DAC values for each line. Channels 1 and 2 have a negative slope, because those galvos move in the opposite direction. |
| [`fit_hamamatsu.p`](../command_center/fit_hamamatsu.p) | `[poly_from_fit x 3]` | A quadratic polynomial that changes a DAC value to a line number. The degree is 2. Thus `poly_from_fit.invert()` can calculate the inverse directly. |
| [`fit_calibration.p`](../command_center/fit_calibration.p) | `[poly_from_fit x 3]` | A quadratic polynomial that changes a DAC value to a reference position. This puts the three channels in one common coordinate system. |
| [`second_order_results.py`](../command_center/second_order_results.py) | 3 sets of 13 values | Polynomial fits of degree 12 for the second-order curve. There is one array for each laser: `second_488`, `second_560`, and `second_642`. |
| `autocalibration/save*.p` | `{zip name: {member name: array}}` | Line data from the recorded zip archives. The file `save.p` contains the most recent data. The files `save_old*.p` contain data from earlier procedures. |

The function `get_calibrations()` reads the two `fit_*.p` files. The function
`calculate_calibrated_galvo` reads `ham_cal.p`. It looks in the current
directory first. Then it looks in the directory `autocalibration/`. Thus the
function operates from the two locations.

The files `fit_calibration.p` and `fit_hamamatsu.p` contain `poly_from_fit`
objects in the pickle format. The module `continuous_calibration` must be
available with this name to read these files. If you change the name of this
class or move it, Python cannot read the two files.

## How to measure the data

### Position map

1. The script
   [`auto_make_galvo_map.py`](../command_center/autocalibration/auto_make_galvo_map.py)
   sets an archive name. Then, for each channel, it moves the galvo through the
   full `uint16` range in steps of 256. It keeps the galvo at the same position
   for one full frame and opens the AOTF fully. It sends one trigger for each
   step.
2. The script
   [`parse_galvo_map.py`](../command_center/autocalibration/parse_galvo_map.py)
   reads the zip archives. It decompresses each frame. Then it calculates the
   mean along `axis=1` to get one line profile. It writes the result to
   `save.p`.
3. The notebook
   [`parse_galvo_map_2.ipynb`](../command_center/autocalibration/parse_galvo_map_2.ipynb)
   finds the illuminated line for each DAC value. Then it calculates the fits.
   This makes `ham_cal.p` and `fit_hamamatsu.p`.

### Second-order curve

1. The script
   [`auto_record_2nd.py`](../command_center/autocalibration/auto_record_2nd.py)
   makes a wavetable with the current correction. Then it records N frames for
   each channel. The galvo moves during the record operation. Thus the data
   shows the curve in operational conditions.
2. The script [`parse_2nd.py`](../command_center/autocalibration/parse_2nd.py)
   is the same as `parse_galvo_map.py`. But it calculates the mean along
   `axis=0` and not `axis=1`. This scan changes in the other direction.
3. Put the new coefficients in
   [`second_order_results.py`](../command_center/second_order_results.py). Also
   put them in the `correction_test` dictionaries in the scripts and the
   notebook.

The two parse scripts ignore all archives with `DEFAULT` in the name. These
archives contain the frames from the time before the first archive name. See
[frame-protocol.md](frame-protocol.md#how-to-set-the-archive-name-on-http-port-8090).

### Phase offset between two channels

The function `calculate_signal_offset(s1, s2)` measures the difference in phase
between the line data of two channels. It gives the difference for each line.
The function does these steps:

- Moves a window of `window_size` (100) lines along the two signals. The window
  moves in steps of `step_size` (25) lines.
- Increases the number of samples in each window by `scale` (100). Thus the
  function can measure an offset of 0.01 line.
- Uses `np.roll` to move the second signal between minus `phase_size` (3) lines
  and plus `phase_size` lines. Keeps the offset with the lowest mean absolute
  difference.
- Calculates a `CubicSpline` through the results of all the windows.

The function `map_calibration(cs_offset, cal_fit, ham_fit)` changes this spline
to line coordinates. It calculates the inverse of the Hamamatsu fit to get a DAC
value. Then it uses the calibration fit to get the common coordinate. Then it
reads the offset spline at that coordinate. Give the result to
`calculate_calibrated_galvo` as the `time_offset` parameter.

## The class poly_from_fit

This class contains `np.polyfit` and adds an `invert()` method:

```python
p = poly_from_fit.fit(x, y, d)   # Makes a new object
p(x)                             # Calculates the value
p.invert(y)                      # Calculates the inverse. The degree must be 0, 1, or 2.
p.d                              # Gives the degree
```

For degree 2, `invert` gives the **two** roots of the quadratic equation in the
sequence `[+, -]`. Thus the calling code selects the root that it needs. The
parameter `zero` of `map_calibration` makes this selection. For a degree of more
than 2, `invert` gives a `NotImplementedError`. This is why the fits from a DAC
value to a line number have a degree of 2. The fit in the other direction has a
degree of 3.

Use `poly_from_fit.fit(x, y, d)` on the class to make a new object. On an
object, the attribute `fit` contains the coefficients.

## Output of the calibration

When a channel is calibrated, write its wavetable to the file
`stage_control/<wavelength>.txt`. The format is hexadecimal values with commas
between them. The method `DACControl.load_defaults()` reads this file at each
start of `run.py`. These three files keep all the calibration data. Use the
procedure in this document to make new files. Do not change the files manually.
