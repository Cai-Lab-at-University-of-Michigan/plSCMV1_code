#pragma once

/*
 * config.h: the settings of the capture application.
 *
 * The application reads the settings one time, at the start. It reads four
 * layers. A later layer replaces an earlier layer:
 *
 *   1. The default values in this file. The application operates correctly
 *      with no configuration.
 *   2. The file cameraman.ini. The file is in the directory of the executable
 *      file. Use --config=<path> or PLSCM_CONFIG to select a different file.
 *   3. The environment variables. The name is PLSCM_ and the name of the
 *      setting in capital letters. Example: PLSCM_SERVER_HOST.
 *   4. The command line. The format is --<name>=<value>.
 *
 * WinMain calls load_config one time, before it starts any thread. Then it
 * gives a const reference to each thread. No thread writes to the structure.
 * Thus no thread needs a lock. This is important, because the capture thread
 * operates at REALTIME_PRIORITY_CLASS. It must not wait.
 *
 * If a value is not correct, load_config shows a message and stops the
 * application with the exit code 2. This agrees with sndif_server. See
 * docs/operations.md.
 *
 * See docs/cameraman-demo-mode.md.
 */

#include <cstdint>
#include <string>

 // The largest number of cameras. This value gives the size of the arrays in
 // the application. The setting camera_count must not be larger than this
 // value. A C++ array cannot have a size from a file. Thus the array is always
 // this size, and camera_count gives the number of loops.
#define MAX_CAMERAS 8

struct Config {
	// ---- Network ----
	// The address of the storage host. PLSCM_SERVER_HOST
	std::string server_host;
	// The TCP port of the storage host. PLSCM_SERVER_PORT
	uint16_t    server_port;
	// If this value is false, the application does not open a connection. It
	// makes the frames and the preview only. PLSCM_SEND_ENABLE
	bool        send_enable;
	// The zstd compression level. Level 1 is the fastest. PLSCM_ZSTD_LEVEL
	int         zstd_level;

	// ---- Frames and threads ----
	// The number of cameras. PLSCM_CAMERA_COUNT
	uint32_t    camera_count;
	// The width of one frame in pixels. PLSCM_FRAME_WIDTH
	uint32_t    frame_width;
	// The height of one frame in pixels. PLSCM_FRAME_HEIGHT
	uint32_t    frame_height;
	// The number of bytes for each pixel. The value must be 2.
	// PLSCM_FRAME_BYTES_PER_PX
	uint32_t    frame_bytes_per_px;
	// The number of IO threads for each camera. PLSCM_IO_THREADS
	uint32_t    io_threads;
	// The number of frame buffers in the camera driver. PLSCM_DCAM_BUFFERS
	uint32_t    dcam_buffers;
	// The largest number of frames in the queue of one camera. The capture
	// thread releases a new frame if the queue is longer. The value 0 gives no
	// limit. PLSCM_MAX_QUEUE_DEPTH
	uint32_t    max_queue_depth;

	// ---- Camera timing ----
	// The line interval of the rolling shutter in seconds. PLSCM_H_INTERVAL
	double      h_interval;
	// The exposure time of one line in seconds. PLSCM_EXPOSURE_TIME
	double      exposure_time;
	// The period of the output triggers in seconds. PLSCM_TRIGGER_INTERVAL
	double      trigger_interval;
	// The number of lead-in lines before the first sensor line. PLSCM_HSYNC
	uint32_t    hsync;

	// ---- Preview ----
	// The preview is this number of times smaller than the frame.
	// PLSCM_PREVIEW_SCALE
	uint32_t    preview_scale;

	// ---- Demo mode ----
	// If this value is true, the application makes its own frames. It does not
	// open a camera and it does not load dcamapi.dll. PLSCM_DEMO_MODE
	bool        demo_mode;
	// The time between two demo frames in milliseconds. PLSCM_DEMO_INTERVAL_MS
	uint32_t    demo_interval_ms;
	// The contents of the demo frames: "bars", "noise", or "file".
	// PLSCM_DEMO_PATTERN
	std::string demo_pattern;
	// The frame file for the pattern "file". PLSCM_DEMO_FILE
	std::string demo_file;
	// The number of bits of noise, from 1 to 16. This value controls the
	// compression ratio. 16 gives a ratio near 1. 4 gives a high ratio.
	// PLSCM_DEMO_NOISE_BITS
	uint32_t    demo_noise_bits;
	// The value of expid in the header of each demo frame. The storage host
	// writes this value in its log. Thus you can find the demo frames.
	// PLSCM_DEMO_EXPID
	uint32_t    demo_expid;

	// ---- Derived values. load_config calculates them. ----
	// The number of bytes of one frame before compression.
	size_t      frame_bytes;
	// The width of one preview panel in pixels.
	uint32_t    panel_width;
	// The height of one preview panel in pixels.
	uint32_t    panel_height;
	// The number of bytes of one row of the shared RGB preview area. A Windows
	// bitmap of 24 bits needs a row of a multiple of 4 bytes.
	size_t      preview_stride;
	// The number of bytes of the shared RGB preview area.
	size_t      preview_bytes;
};

// Reads the four layers and gives the settings. Stops the application with the
// exit code 2 if a value is not correct.
Config load_config();

// Writes every setting in the console. Also writes the origin of each value.
void show_config(const Config& c);
