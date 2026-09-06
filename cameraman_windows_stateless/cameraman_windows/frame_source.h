#pragma once

/*
 * frame_source.h: the origin of the frames.
 *
 * The capture thread does not know where a frame comes from. It asks a
 * FrameSource for the next frame. There are two sources:
 *
 *   DcamFrameSource   Reads a Hamamatsu camera with DCAM-API. This is the
 *                     source for the microscope.
 *   DemoFrameSource   Makes its own frames. This is the source for a test on a
 *                     PC with no camera. The setting demo_mode selects it.
 *
 * The demo source does not call DCAM-API. The linker option
 * /DELAYLOAD:dcamapi.dll gives the result that Windows loads dcamapi.dll at the
 * first DCAM call only. Thus the application also operates on a PC that does
 * not have the DCAM-API runtime.
 *
 * Everything after the source is the same code in the two modes: the queue,
 * the compression, the network, the preview, and the window. Thus a test in
 * demo mode is a test of the application.
 *
 * See docs/cameraman-demo-mode.md.
 */

#include <cstdint>
#include <mutex>

#include "config.h"

 // The camera handle of DCAM-API. This declaration prevents an include of
 // dcamapi4.h in this file.
struct tag_dcam;

// The mutex of the console. cameraman_windows.cpp defines it. Two threads must
// not write a message at the same time.
extern std::mutex cout_mutex;

class FrameSource {
public:
	virtual ~FrameSource() {}

	// Prepares the source. Gives false if the source is not available.
	virtual bool start() = 0;

	// Waits for the frame with the number `index`. The first number is 1. Then
	// writes the frame into `dest`. `dest` has config.frame_bytes bytes.
	// Gives false if no more frames are available.
	virtual bool next_frame(uint32_t index, void* dest) = 0;

	// Stops the source.
	virtual void stop() = 0;
};

// Makes a source that reads a camera. The application must open the camera
// first. See init_api_and_cameras.
FrameSource* make_dcam_source(const Config& cfg, uint8_t camera_id,
	struct tag_dcam* hdcam);

// Makes a source that gives synthetic frames.
FrameSource* make_demo_source(const Config& cfg, uint8_t camera_id);
