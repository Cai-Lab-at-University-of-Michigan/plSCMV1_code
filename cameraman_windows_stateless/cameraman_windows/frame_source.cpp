/*
 * frame_source.cpp: the camera source and the demo source.
 *
 * See frame_source.h and docs/cameraman-demo-mode.md.
 */

#include <windows.h>

#include "dcamapi4.h"
#include "dcamprop.h"
#include "common.h"
#pragma comment(lib,"dcamapi.lib")

#include "frame_source.h"

#include <zstd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

 // ----------------------------------------------------------------
 // Messages

 /// Shows a message and stops the application. The application is a window
 /// application. Thus a message on the console is not sufficient.
static void source_fail(const std::string& text) {
	printf("\nDEMO SOURCE ERROR: %s\n", text.c_str());
	fflush(stdout);
	MessageBoxA(NULL, text.c_str(),
		"cameraman_windows: demo source error", MB_ICONERROR | MB_OK);
	exit(2);
}

// ----------------------------------------------------------------
// The camera source

/// Reads the frames of one Hamamatsu camera with DCAM-API.
class DcamFrameSource : public FrameSource {
public:
	DcamFrameSource(const Config& cfg, uint8_t camera_id, HDCAM hdcam)
		: cfg_(cfg), id_(camera_id), hdcam_(hdcam) {
		memset(&bufframe_, 0, sizeof(bufframe_));
	}

	bool start() {
		bufframe_.size = sizeof(DCAMBUF_FRAME);
		bufframe_.iKind = 0;
		bufframe_.option = 0;
		bufframe_.iFrame = -1;
		bufframe_.buf = NULL;
		bufframe_.rowbytes = (int32)(cfg_.frame_width * cfg_.frame_bytes_per_px);
		bufframe_.type = DCAM_PIXELTYPE_NONE;
		bufframe_.width = (int32)cfg_.frame_width;
		bufframe_.height = (int32)cfg_.frame_height;
		bufframe_.left = 0;
		bufframe_.top = 0;
		bufframe_.timestamp.sec = 0;
		bufframe_.timestamp.microsec = 0;
		bufframe_.framestamp = 0;
		bufframe_.camerastamp = 0;

		DCAMERR err = dcamcap_start(hdcam_, DCAMCAP_START_SEQUENCE);
		if (failed(err)) {
			dcamcon_show_dcamerr(hdcam_, err, "dcamcap_start()");
			return false;
		}
		return true;
	}

	bool next_frame(uint32_t index, void* dest) {
		DCAMERR err;

		// Wait until the camera has the frame with this number.
		DCAMCAP_TRANSFERINFO info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		info.iKind = DCAMCAP_TRANSFERKIND_FRAME;

		do {
			err = dcamcap_transferinfo(hdcam_, &info);
			if (failed(err)) {
				cout_mutex.lock();
				dcamcon_show_dcamerr(hdcam_, err, "dcamcap_transferinfo()");
				cout_mutex.unlock();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (info.nFrameCount < (int32)index);

		cout_mutex.lock();
		printf("[%d][#%09u] Got %d frames, last %d\n",
			(int)id_, index, info.nFrameCount, info.nNewestFrameIndex);
		printf("[%d] GAP = %d\n", (int)id_, (int)info.nFrameCount - (int)index);
		cout_mutex.unlock();

		// Copy the frame into the buffer of the caller.
		bufframe_.buf = dest;
		bufframe_.iFrame = (int32)((index - 1) % cfg_.dcam_buffers);

		do {
			err = dcambuf_copyframe(hdcam_, &bufframe_);
			if (failed(err) && err != DCAMERR_BUSY) {
				cout_mutex.lock();
				dcamcon_show_dcamerr(hdcam_, err, "dcambuf_copyframe()");
				cout_mutex.unlock();
			}
		} while (err == DCAMERR_BUSY);

		return true;
	}

	void stop() {
		dcamcap_stop(hdcam_);
	}

private:
	const Config& cfg_;
	uint8_t       id_;
	HDCAM         hdcam_;
	DCAMBUF_FRAME bufframe_;
};

FrameSource* make_dcam_source(const Config& cfg, uint8_t camera_id,
	struct tag_dcam* hdcam) {
	return new DcamFrameSource(cfg, camera_id, (HDCAM)hdcam);
}

// ----------------------------------------------------------------
// The demo source

// The values of a real sensor are in a limited range. These two values give a
// dark level and the range of the noise above it.
#define DEMO_DARK 100
#define DEMO_SPAN 3900

// The largest value of the bright band of the pattern "bars".
#define DEMO_BAND_PEAK 50000

/// Makes synthetic frames. The application uses this source in demo mode.
class DemoFrameSource : public FrameSource {
public:
	DemoFrameSource(const Config& cfg, uint8_t camera_id)
		: cfg_(cfg), id_(camera_id), base_(NULL),
		state_(0x9E3779B9u + camera_id * 0x85EBCA6Bu),
		dark_(0), bright_(0), block_(1), marker_(1) {
	}

	bool start() {
		// A xorshift generator with the state 0 gives only zeros.
		if (state_ == 0) state_ = 1;

		base_ = (uint16_t*)malloc(cfg_.frame_bytes);
		if (base_ == NULL) {
			source_fail("The application cannot get the memory for the demo"
				" frame of camera " + std::to_string((int)id_) + ".");
			return false;
		}

		if (cfg_.demo_pattern == "file") {
			load_from_file();
		}
		else {
			fill_with_noise();
			if (cfg_.demo_pattern == "bars") add_band();
		}

		find_dark_and_bright();

		// The size of the blocks of the frame number and of the marker. The
		// numbers 64 and 12 keep the two marks visible in the small preview.
		block_ = cfg_.frame_width / 64;
		if (block_ < 1) block_ = 1;
		marker_ = cfg_.frame_width / 12;
		if (marker_ < 1) marker_ = 1;

		deadline_ = std::chrono::steady_clock::now();

		cout_mutex.lock();
		printf("[%d] Demo source ready. pattern=%s dark=%u bright=%u\n",
			(int)id_, cfg_.demo_pattern.c_str(),
			(unsigned)dark_, (unsigned)bright_);
		cout_mutex.unlock();
		return true;
	}

	bool next_frame(uint32_t index, void* dest) {
		// Wait for the next time. Use a deadline and not a sleep of a fixed
		// time. Thus a slow frame does not make a delay that grows.
		if (cfg_.demo_interval_ms > 0) {
			deadline_ += std::chrono::milliseconds(cfg_.demo_interval_ms);
			const std::chrono::steady_clock::time_point now =
				std::chrono::steady_clock::now();
			if (deadline_ < now) deadline_ = now;
			std::this_thread::sleep_until(deadline_);
		}

		memcpy(dest, base_, cfg_.frame_bytes);
		stamp((uint16_t*)dest, index);

		if (index == 1 || index % 100 == 0) {
			cout_mutex.lock();
			printf("[%d][#%09u] Demo frame\n", (int)id_, index);
			cout_mutex.unlock();
		}
		return true;
	}

	void stop() {
		free(base_);
		base_ = NULL;
	}

private:
	/// A fast pseudo-random generator (xorshift32). The demo does not need a
	/// generator of high quality. It needs speed.
	uint32_t next_random() {
		state_ ^= state_ << 13;
		state_ ^= state_ >> 17;
		state_ ^= state_ << 5;
		return state_;
	}

	/// Fills the base frame with noise. The setting demo_noise_bits gives the
	/// number of different values. Fewer values give a higher compression
	/// ratio.
	void fill_with_noise() {
		uint32_t levels = 1u << cfg_.demo_noise_bits;
		if (levels > DEMO_SPAN) levels = DEMO_SPAN;
		const uint32_t step = DEMO_SPAN / levels;

		const size_t count = (size_t)cfg_.frame_width * cfg_.frame_height;
		for (size_t i = 0; i < count; i++) {
			base_[i] = (uint16_t)(DEMO_DARK + (next_random() % levels) * step);
		}
	}

	/// Adds one bright horizontal band. The microscope is a line-scanning
	/// microscope. Thus a band is like the real data. Each camera has a
	/// different position. Thus you can see immediately if the channels are not
	/// in the correct sequence.
	void add_band() {
		const uint32_t center =
			cfg_.frame_height * (id_ + 1) / (cfg_.camera_count + 1);
		uint32_t half = cfg_.frame_height / 16;
		if (half < 1) half = 1;

		for (uint32_t y = 0; y < cfg_.frame_height; y++) {
			const uint32_t distance = (y > center) ? (y - center) : (center - y);
			if (distance >= half) continue;

			// A triangle gives the shape of the band.
			const double weight = 1.0 - ((double)distance / (double)half);
			const uint32_t add = (uint32_t)(weight * DEMO_BAND_PEAK);

			uint16_t* row = base_ + (size_t)y * cfg_.frame_width;
			for (uint32_t x = 0; x < cfg_.frame_width; x++) {
				const uint32_t value = row[x] + add;
				row[x] = (uint16_t)(value > 65535 ? 65535 : value);
			}
		}
	}

	/// Reads the base frame from a file. The file is one member of a zip
	/// archive of the storage host, or a raw frame. This pattern gives the most
	/// exact compression ratio, because the data is real data.
	void load_from_file() {
		std::ifstream file(cfg_.demo_file.c_str(),
			std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			source_fail("The application cannot open the demo file:\n" +
				cfg_.demo_file);
		}

		const std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		std::vector<char> raw((size_t)size);
		if (size > 0) file.read(&raw[0], size);
		file.close();

		if ((size_t)size == cfg_.frame_bytes) {
			// The file is a frame without compression.
			memcpy(base_, &raw[0], cfg_.frame_bytes);
			return;
		}

		// The file has zstd compression. This is the format in the archives of
		// the storage host.
		const size_t result =
			ZSTD_decompress(base_, cfg_.frame_bytes, &raw[0], (size_t)size);
		if (ZSTD_isError(result)) {
			source_fail("The application cannot read the demo file:\n" +
				cfg_.demo_file + "\n\nThe file is not a frame of " +
				std::to_string(cfg_.frame_bytes) + " bytes, and zstd gives"
				" this message: " + ZSTD_getErrorName(result));
		}
		if (result != cfg_.frame_bytes) {
			source_fail("The demo file has the wrong size:\n" + cfg_.demo_file +
				"\n\nThe file gives " + std::to_string(result) +
				" bytes. The settings need " + std::to_string(cfg_.frame_bytes) +
				" bytes. Set frame_width and frame_height to the size of this"
				" frame.");
		}
	}

	/// Finds the smallest value and the largest value of the base frame. The
	/// marks use these two values. Thus a mark is visible, but it does not
	/// change the range of the preview.
	void find_dark_and_bright() {
		const size_t count = (size_t)cfg_.frame_width * cfg_.frame_height;
		dark_ = 65535;
		bright_ = 0;
		for (size_t i = 0; i < count; i++) {
			if (base_[i] < dark_) dark_ = base_[i];
			if (base_[i] > bright_) bright_ = base_[i];
		}
		// The two values must be different. If they are equal, the code that
		// changes the preview to 8 bits divides by zero.
		if (dark_ == bright_) {
			if (bright_ < 65535) bright_ = 65535; else dark_ = 0;
		}
	}

	/// Draws a filled rectangle in the frame of the caller.
	void draw_block(uint16_t* frame, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, uint16_t value) {
		for (uint32_t j = 0; j < height; j++) {
			const uint32_t row_y = y + j;
			if (row_y >= cfg_.frame_height) return;
			uint16_t* row = frame + (size_t)row_y * cfg_.frame_width;
			for (uint32_t i = 0; i < width; i++) {
				const uint32_t column = x + i;
				if (column >= cfg_.frame_width) break;
				row[column] = value;
			}
		}
	}

	/// Writes the two marks on one frame.
	///
	/// The first mark is the number of the frame. It is 32 blocks at the top.
	/// A bright block is a bit with the value 1. Thus you can find the number
	/// of a frame in a saved archive.
	///
	/// The second mark is a square that moves. Its position changes with each
	/// frame. Thus you can see in the preview that the frames are new. If the
	/// square stops, the capture path stopped.
	void stamp(uint16_t* frame, uint32_t index) {
		for (uint32_t bit = 0; bit < 32; bit++) {
			const bool one = ((index >> (31 - bit)) & 1u) != 0;
			draw_block(frame, bit * block_, 0, block_, block_,
				one ? bright_ : dark_);
		}

		if (cfg_.frame_width > marker_ && cfg_.frame_height > marker_ * 2) {
			const uint32_t travel = cfg_.frame_width - marker_;
			const uint32_t x = (index * (marker_ / 4 + 1)) % travel;
			const uint32_t y = cfg_.frame_height - (marker_ * 2);
			draw_block(frame, x, y, marker_, marker_, bright_);
		}
	}

	const Config& cfg_;
	uint8_t   id_;
	uint16_t* base_;
	uint32_t  state_;
	uint16_t  dark_;
	uint16_t  bright_;
	uint32_t  block_;
	uint32_t  marker_;
	std::chrono::steady_clock::time_point deadline_;
};

FrameSource* make_demo_source(const Config& cfg, uint8_t camera_id) {
	return new DemoFrameSource(cfg, camera_id);
}
