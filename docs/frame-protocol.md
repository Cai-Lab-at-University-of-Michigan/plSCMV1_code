# Frame protocol (SNDIF)

The SNDIF protocol connects the camera PC to the storage host. The protocol uses
two ports. A TCP port carries the frame data. An HTTP port sets the name of the
archive that receives the frames.

The transmitter is the function `send_buffer_over_ip` in
[`cameraman_windows.cpp`](../cameraman_windows_stateless/cameraman_windows/cameraman_windows.cpp).
The receiver is [`sndif_server/src/main.rs`](../sndif_server/src/main.rs). The
structure is in
[`sendme.h`](../cameraman_windows_stateless/cameraman_windows/sendme.h).

## Frame input on TCP port 8080

The protocol sends **one frame on each connection**. The client makes a
connection. Then it writes a header of 32 bytes. Then it writes the compressed
payload. Then it closes one half of the socket and disconnects.

The server reads the payload with `read_to_end`. Thus the half-close operation
gives the end of the frame. The payload has no length value in front of it. You
cannot send two frames on one connection.

### Header format (32 bytes, little-endian)

| Offset | Size | Value | Remarks |
|---:|---:|---|---|
| 0 | 1 | `cameraid` | A number from 0 to 2. Selects the writer thread. |
| 1 | 3 | padding | The transmitter does not write these bytes. The receiver ignores them. |
| 4 | 4 | `frameid` | A counter for each camera. The first value is 1. |
| 8 | 4 | `burstid` | The value is always 0. See [Unused values](#unused-values). |
| 12 | 4 | `expid` | The value is always 0. See [Unused values](#unused-values). |
| 16 | 8 | `timecode` | Milliseconds after the start of the Unix epoch. The camera PC gives this value. |
| 24 | 4 | `payload_size` | The **uncompressed** number of bytes: 2304 x 2304 x 2 = 10,616,832. |
| 28 | 4 | padding | The transmitter does not write these bytes. The receiver does not read them. |

### Why the header is 32 bytes

The values in the header add up to 25 bytes. But the transmitter sends
`sizeof(sendme)` bytes from a C structure. Thus the alignment rules of the
compiler give the format on the line. With the default 8-byte alignment of
MSVC, the layout is:

```text
byte:  0    1  2  3    4        8        12       16               24        28
      [id] [pad pad ] [frameid][burstid][expid  ][timecode       ][payload ][pad ]
       u8   ^^^^^^^^^  u32      u32      u32      u64              u32       ^^^^
            aligns u32 to 4                       aligns u64 to 8            makes
                                                                             sizeof
                                                                             a
                                                                             multiple
                                                                             of 8
```

The space of 3 bytes after `cameraid` aligns the block of `u32` values. The
4 bytes at the end make `sizeof(struct sendme)` a multiple of 8. The alignment
of the structure is 8 bytes. This is why the receiver reads a buffer of 32 bytes
and then ignores three bytes after the camera number.

**CAUTION: The format on the line changes with the ABI.** If you change the
compiler, change the target architecture, or add a value to `sendme`, the format
changes. The offsets in the receiver do not change automatically. If you change
`sendme.h`, you must also change the parser in `main.rs`. Do the two changes in
the same commit.

### Payload

The payload is the 16-bit sensor data. The transmitter compresses the data with
**zstd at level 1** (`ZSTD_compress(..., 1)`). Level 1 gives the highest speed.
Five IO threads for each camera use the compressor continuously. The limit is
the data rate of the compressor and not the bandwidth of the network.

The transmitter gets a buffer of `2 x payload_size` bytes for the compressed
data.

The server writes one line in the log for each frame:

```text
camera=1; frame=0-0-42; time=1685500000123; buffer_size=10616832; Got buffer: 2201984 (4.82X)
```

The value `payload_size` in the header gives the size of the uncompressed frame.
The number of bytes in the log gives the size of the data that the server
received. The relation between the two numbers is the compression ratio.

Monitor the compression ratio. A high ratio tells you that a camera has no
signal, because a frame of zeros compresses very well. A high ratio can also
tell you that a channel is at its maximum value.

## How to set the archive name on HTTP port 8090

This port has one endpoint. The endpoint sets the name of the next archive.

```http
GET /<name>
```

The server accepts all GET requests. The server takes the target of the request
and removes all the `/` characters. Then it sends the result to the three writer
threads. Then it replies with `200 OK` and an empty body. Each writer thread
closes its zip file. Then it opens the file `<name>_ch<N>.zip` in the work
directory of the server.

```bash
curl "http://sndif.cai-lab.org:8090/coord_+1.234,-5.678"
# The writers change to coord_+1.234,-5.678_ch0.zip, _ch1.zip, and _ch2.zip
```

The experiment scripts use this function:

```python
def set_string(in_str):
    url = f"http://10.156.2.28:8090/{in_str}"
    return urllib.request.urlopen(url).read().decode()
```

Obey these rules:

- **The name applies to all three cameras.** The three channels change to the
  new archive together.
- **The server removes the `/` characters. It does not reject them.** The target
  `/a/b` gives the name `ab`.
- **The change of archive is not synchronous with the capture operation.** The
  frames that are already in the queue go to the file that their writer thread
  has open. 

## Archive format

The server writes each zip file with `CompressionMethod::Stored`. The payload
has zstd compression already. A second compression operation would only use more
CPU time. The names of the members are:

```text
frame_<frameid>_t<timecode>.zstd
```

To read one frame, decompress the data and change the shape of the array:

```python
import zipfile, zstd, numpy as np

with zipfile.ZipFile("scan_1685500000_ch0.zip") as zf:
    with zf.open(zf.namelist()[0]) as f:
        raw = zstd.ZSTD_uncompress(f.read())
        frame = np.frombuffer(raw, dtype=np.uint16).reshape(2304, 2304)
```

The two calibration scripts use this loop and then calculate a mean value.
[`parse_galvo_map.py`](../command_center/autocalibration/parse_galvo_map.py)
calculates the mean along `axis=1`.
[`parse_2nd.py`](../command_center/autocalibration/parse_2nd.py) calculates the
mean along `axis=0`. This is the only difference between the two scripts.

## Test data generator

[`sndif_test_client/`](../sndif_test_client/) makes test data for this protocol.
The generator starts 6 threads. There are 2 threads for each of the 3 cameras.
Each thread sends 500 frames. It uses one TCP connection for each frame.

Each thread sends a different range of frame numbers. Thus the names of the
members in the zip archive are unique.

The payload is a buffer of zeros with the size of one uncompressed frame. The
generator does not compress the data. Thus the server receives 10,616,832 bytes
for each frame and reports a compression ratio of 1X. This is the condition with
the highest load.

An environment variable sets each value in the configuration of the generator.
This includes the address of the server, the number of frames, and the size of
a frame. See
[operations.md](operations.md#configuration-of-the-two-rust-programs).
