/*
 * cameraman_windows: captures the camera data and sends the frames.
 *
 * This application operates on the camera PC. It controls three Hamamatsu
 * cameras with DCAM-API. It compresses each frame and sends it to the storage
 * host. It also shows a live preview with three panels.
 *
 * The name "stateless" tells you that this application keeps no experiment
 * data. It operates continuously and sends each frame immediately. The storage
 * host puts each frame in the correct archive. To do this, the storage host
 * uses the last name that it received on its control port. See
 * docs/frame-protocol.md.
 *
 * SETTINGS. The application reads its settings at the start. It does not use
 * constants in the code. See config.h for the four layers: the default values,
 * the file cameraman.ini, the PLSCM_ environment variables, and the command
 * line. WinMain calls load_config one time, before it starts any thread. Then
 * no thread writes to the settings. Thus no thread needs a lock.
 *
 * DEMO MODE. With the setting demo_mode the application makes its own frames.
 * It does not open a camera, and Windows does not load dcamapi.dll. Thus you
 * can test the application on a PC that has no camera and no DCAM-API runtime.
 * See frame_source.h and docs/cameraman-demo-mode.md.
 *
 * THREADS. The function camera_thread_main starts these threads for each
 * camera:
 *
 *   camera_thread_main    The capture thread. It operates at
 *                         REALTIME_PRIORITY_CLASS. It gets a buffer with
 *                         malloc. Then it asks the FrameSource for the next
 *                         frame. Then it adds a time value in milliseconds.
 *                         Last, it puts the data in a queue with a mutex.
 *   io_thread_loop        The IO threads (io_threads). Each thread gets a
 *                         buffer from the queue and copies it to the preview
 *                         area. Then it compresses the buffer with zstd. Then
 *                         it opens a new TCP connection and sends the header
 *                         and the payload. Last, it closes the connection and
 *                         releases the buffer. A new connection for each frame
 *                         keeps the transmitter stateless. It also lets the
 *                         receiver use the half-close operation to find the end
 *                         of each frame.
 *   preview_update_thread The preview thread. It decreases the size of the
 *                         newest frame by preview_scale. Then it changes the
 *                         data to 8-bit with the minimum value and the maximum
 *                         value of that frame. Then it writes the result into
 *                         the shared RGB preview area at the horizontal offset
 *                         of this camera.
 *
 * WinMain controls the Win32 message loop. It draws one preview panel for each
 * camera. Below the panels it shows the minimum value, the maximum value, and
 * the frame rate.
 *
 * FORMAT ON THE LINE. The application sends a header of 32 bytes (struct
 * sendme, see sendme.h). Then it sends the frame with zstd compression. The
 * values in the header add up to 25 bytes. But the header is 32 bytes, because
 * the application sends a C structure. Thus the alignment padding is part of
 * the format. CAUTION: THE FORMAT CHANGES WITH THE ABI. If you change
 * sendme.h, you must also change the parser in sndif_server/src/main.rs. Do the
 * two changes in the same commit.
 *
 * TIMING. The cameras use a rolling shutter. The line interval (h_interval) is
 * 4.868 microseconds. The camera has 150 lead-in lines (hsync). The control PC
 * sends the galvo tables and the AOTF tables. The system uses one value for
 * each line. Thus each table has 150 + frame_height + 100 = 2554 values.
 *
 * TO STOP THIS APPLICATION, close the preview window. Then stop the process.
 * Ctrl-C in the console window also stops it.
 *
 * See docs/architecture.md and docs/frame-protocol.md.
 */

#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

///// dcampapi imports
#include "dcamapi4.h"
#include "dcamprop.h"
#include "common.h"
#pragma comment(lib,"dcamapi.lib")

// Windows loads dcamapi.dll at the first DCAM call and not at the start of the
// application. In demo mode the application makes no DCAM call. Thus it also
// operates on a PC that does not have the DCAM-API runtime.
#pragma comment(linker, "/DELAYLOAD:dcamapi.dll")
///// ----------------------------

#include "util.h"
#include "config.h"
#include "frame_source.h"
#include <zstd.h> // Installed through VCpkg

#include<chrono>
#include<iostream>
#include<fstream>
#include<thread>
#include<format>
#include<future>
#include<mutex>
#include<tuple>
#include<queue>
#include<string>
#include<sstream>
#include<cstdlib>
#include<numeric>

///// Headers and libs for win32 socket support
#include <atlstr.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")
///// -----------------------------

///// headers for win32 inet libs
#include <urlmon.h>
#include <wininet.h>
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")
///// -----------------------------

#include "sendme.h"

// The settings. WinMain writes this variable one time, before it starts any
// thread. No other function writes it. Thus no lock is necessary.
Config g_config;

// The shared RGB preview area. The preview threads write it. WM_PAINT reads
// it. One row has g_config.preview_stride bytes.
uint8_t* preview_buffer = NULL;
uint16_t camera_min_vals[MAX_CAMERAS], camera_max_vals[MAX_CAMERAS];

std::mutex cout_mutex;
std::mutex live_preview_mutex;

struct RGB_Tuple {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

std::string download_url(std::string url) {
	std::string out = "";
	ULONG bytesRead;
	IStream* stream;
	char* buff = (char*)malloc(100);

	DeleteUrlCacheEntry(convert_narrow_to_wide_string(url).c_str());
	if (URLOpenBlockingStreamA(0, url.c_str(), &stream, 0, 0)) return "";

	do {
		stream->Read(buff, 100, &bytesRead);
		out.append(buff, bytesRead);
	} while (bytesRead > 0);

	free(buff);
	stream->Release();

	return out;
}

/*
void wait_for_trigger() {
	while (true) {
		std::string res = download_url(GET_TRIGGER_URL);
		if (res.size() > 0 && res.at(0) == 'T') break;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}
*/

void assignSettings(HDCAM hdcam, const Config& cfg) {
	std::cout << "Calling camera settings: " << hdcam << std::endl;
	dcamprop_setvalue(hdcam, DCAM_IDPROP_SENSORMODE, DCAMPROP_SENSORMODE__PROGRESSIVE); // set progressive mode (=2)
	dcamprop_setvalue(hdcam, DCAM_IDPROP_READOUTSPEED, 3); // set fast mode (=3)
	dcamprop_setvalue(hdcam, DCAM_IDPROP_SENSORCOOLER, DCAMPROP_SENSORCOOLER__ON); // sensor cooler on
	dcamprop_setvalue(hdcam, DCAM_IDPROP_BINNING, DCAMPROP_BINNING__1); // binning for test

	dcamprop_setvalue(hdcam, DCAM_IDPROP_EXPOSURETIME, cfg.exposure_time); // force exposure time
	dcamprop_setvalue(hdcam, DCAM_IDPROP_INTERNAL_LINEINTERVAL, cfg.h_interval);
	dcamprop_setvalue(hdcam, DCAM_IDPROP_TRIGGERACTIVE, DCAMPROP_TRIGGERACTIVE__EDGE);
	dcamprop_setvalue(hdcam, DCAM_IDPROP_TRIGGER_MODE, DCAMPROP_TRIGGER_MODE__NORMAL); // CHECK
	//dcamprop_setvalue(hdcam, DCAM_IDPROP_INTERNALLINESPEED, 1.335347432);

	// Subarray optionally
	if (0) {
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYMODE, DCAMPROP_MODE__OFF);
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYHPOS, 128);
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYHSIZE, 256);
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYVPOS, 128);
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYVSIZE, 256);
		dcamprop_setvalue(hdcam, DCAM_IDPROP_SUBARRAYMODE, DCAMPROP_MODE__ON);
	}

	dcamprop_setvalue(hdcam, DCAM_IDPROP_OUTPUTTRIGGER_PREHSYNCCOUNT, cfg.hsync); // PRESYNC
	dcamprop_setvalue(hdcam, DCAM_IDPROP_OUTPUTTRIGGER_DELAY, 0); // Delay output trigger

	// -- Trigger 0 --
	dcamprop_setvalue(hdcam, 0x001c0110, DCAMPROP_OUTPUTTRIGGER_SOURCE__TRIGGER); // source
	dcamprop_setvalue(hdcam, 0x001c0120, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0130, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0150, cfg.trigger_interval); // period
	dcamprop_setvalue(hdcam, 0x001c0160, DCAMPROP_OUTPUTTRIGGER_KIND__PROGRAMABLE); //kind

	// -- Trigger 1 -- 
	dcamprop_setvalue(hdcam, 0x001c0210, DCAMPROP_OUTPUTTRIGGER_SOURCE__TRIGGER); // source
	dcamprop_setvalue(hdcam, 0x001c0220, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0230, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0250, cfg.trigger_interval); // period
	dcamprop_setvalue(hdcam, 0x001c0260, DCAMPROP_OUTPUTTRIGGER_KIND__PROGRAMABLE); //kind

	// -- Trigger 2 --
	dcamprop_setvalue(hdcam, 0x001c0310, DCAMPROP_OUTPUTTRIGGER_SOURCE__HSYNC); // source
	dcamprop_setvalue(hdcam, 0x001c0320, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0330, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0350, cfg.h_interval / 2);  // 0.0000024338); // period
	dcamprop_setvalue(hdcam, 0x001c0360, DCAMPROP_OUTPUTTRIGGER_KIND__PROGRAMABLE); //kind

	// Trigger Settings
	dcamprop_setvalue(hdcam, DCAM_IDPROP_TRIGGERPOLARITY, DCAMPROP_TRIGGERPOLARITY__POSITIVE); // trigger + polarity
	dcamprop_setvalue(hdcam, DCAM_IDPROP_TRIGGERSOURCE, DCAMPROP_TRIGGERSOURCE__EXTERNAL); // trigger 1=internal, 2=external
}

BOOL inline setThreadPriority(int priority) {
	HANDLE current_thread = GetCurrentThread();
	BOOL result = SetThreadPriority(current_thread, priority);
	return result;
}

HDCAM get_camera_by_id(int32 iDevice) {
	DCAMDEV_OPEN devopen;
	memset(&devopen, 0, sizeof(devopen));
	devopen.size = sizeof(devopen);
	devopen.index = iDevice;
	DCAMERR err = dcamdev_open(&devopen);

	if (failed(err)) {
		dcamcon_show_dcamerr((HDCAM)(intptr_t)iDevice, err, "dcamdev_open()", "index is %d\n", iDevice);
		return NULL;
	}

	dcamcon_show_dcamdev_info_detail(devopen.hdcam);
	return devopen.hdcam;
}

/*
 * Sends the full buffer. The function `send` can send less data than the size
 * of the buffer. Thus one call is not sufficient. Gives false on an error.
 */
static bool send_all(SOCKET socket, const char* data, size_t size) {
	size_t sent = 0;
	while (sent < size) {
		size_t remainder = size - sent;
		if (remainder > 0x40000000) remainder = 0x40000000; // The limit of send() is an int.
		const int result = send(socket, data + sent, (int)remainder, 0);
		if (result == SOCKET_ERROR) return false;
		if (result <= 0) return false;
		sent += (size_t)result;
	}
	return true;
}

int send_buffer_over_ip(void* buffer, sendme* tosend, const Config& cfg) {
	WSADATA wsaData;
	struct addrinfo* address = NULL;
	struct addrinfo hints;

	// Compress the frame. ZSTD_compressBound gives the largest size that the
	// compressed data can have.
	const size_t cbuffer_size = ZSTD_compressBound(tosend->payload_size);
	void* cbuffer = malloc(cbuffer_size);
	if (cbuffer == NULL) {
		printf("The application cannot get %zu bytes for the compressed frame.\n",
			cbuffer_size);
		return -1;
	}

	const size_t csize = ZSTD_compress(cbuffer, cbuffer_size, buffer,
		tosend->payload_size, cfg.zstd_level);
	if (ZSTD_isError(csize)) {
		printf("zstd failed: %s\n", ZSTD_getErrorName(csize));
		free(cbuffer);
		return -2;
	}

	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		free(cbuffer);
		return -3;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	// Resolve the server address and port
	iResult = getaddrinfo(cfg.server_host.c_str(),
		std::to_string(cfg.server_port).c_str(), &hints, &address);
	if (iResult != 0) {
		printf("getaddrinfo failed with error: %d\n", iResult);
		free(cbuffer);
		WSACleanup();
		return -4;
	}

	// Create connection socket
	SOCKET ConnectSocket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
	if (ConnectSocket == INVALID_SOCKET) {
		printf("socket failed with error: %ld\n", WSAGetLastError());
		freeaddrinfo(address);
		free(cbuffer);
		WSACleanup();
		return -5;
	}

	// Connect to server.
	if (connect(ConnectSocket, address->ai_addr, (int)address->ai_addrlen) == SOCKET_ERROR) {
		printf("Unable to connect to server!\n");
		closesocket(ConnectSocket);
		freeaddrinfo(address);
		free(cbuffer);
		WSACleanup();
		return -6;
	}

	// Send the header of 32 bytes.
	if (!send_all(ConnectSocket, (const char*)tosend, sizeof(sendme))) {
		printf("Header send failed with error: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		freeaddrinfo(address);
		free(cbuffer);
		WSACleanup();
		return -7;
	}

	// Send the compressed frame.
	if (!send_all(ConnectSocket, (const char*)cbuffer, csize)) {
		printf("Buffer send failed with error: %d\n", WSAGetLastError());
		printf("\tBuffer size: %zu / %zu\n", csize, cbuffer_size);
		closesocket(ConnectSocket);
		freeaddrinfo(address);
		free(cbuffer);
		WSACleanup();
		return -8;
	}

	// shutdown the connection since no more data will be sent. This half-close
	// operation gives the end of the frame to the receiver.
	iResult = shutdown(ConnectSocket, SD_SEND);
	if (iResult == SOCKET_ERROR) {
		printf("shutdown failed with error: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		freeaddrinfo(address);
		free(cbuffer);
		WSACleanup();
		return -9;
	}

	// cleanup
	closesocket(ConnectSocket);
	freeaddrinfo(address);
	free(cbuffer);
	WSACleanup();

	return 1;
}

void io_thread_loop(const Config* cfg_ptr, std::queue<io_tuple>* io_buffer, std::mutex* io_mutex, int* killsignal, std::mutex* preview_mutex, std::queue<void*>* preview_buffer) {
	const Config& cfg = *cfg_ptr;

	for (size_t count = 0; !(*killsignal); count++) { // || (*io_buffer).size() > 0) {
		io_tuple popped = { 0, 0, 0, 0, 0, 0, NULL };

		// Remove object from buffer and store in 'popped'
		(*io_mutex).lock();
		if ((*io_buffer).size() > 0) {
			popped = (*io_buffer).front();
			(*io_buffer).pop();
		}
		(*io_mutex).unlock();

		void* buffer = std::get<6>(popped);
		sendme tosend{
			.cameraid = (uint8_t)std::get<1>(popped),
			.frameid = (uint32_t)std::get<0>(popped),
			.burstid = (uint32_t)std::get<2>(popped),
			.expid = (uint32_t)std::get<3>(popped),
			.timecode = (uint64_t)std::get<4>(popped),
			.payload_size = (uint32_t)std::get<5>(popped)
		};

		if (buffer == NULL) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		// Copy to the preview buffer for GUI
		preview_mutex->lock();
		memcpy(preview_buffer->front(), buffer, cfg.frame_bytes);
		preview_mutex->unlock();

		// rescale buffer
		if (false)
		{
			tosend.payload_size /= 2;
			uint16_t* buffer_cast = (uint16_t*)buffer;
			uint8_t* buffer_processed = (uint8_t*)malloc(tosend.payload_size);
			float tmp = 0;
			for (size_t i = 0; i < tosend.payload_size; i++) {
				tmp = buffer_cast[i];
				tmp = sqrt(tmp);
				if (tmp < 0) tmp = 0;
				if (tmp > 255) tmp = 255;
				buffer_processed[i] = (uint8_t)tmp;
			}
			free(buffer_cast);
			buffer = buffer_processed;
		}

		cout_mutex.lock();
		printf("[%d][#%09d] Frame processing...\n", tosend.cameraid, tosend.frameid);
		cout_mutex.unlock();

		// Write to IP buffer. The setting send_enable = 0 gives a test of the
		// preview and the window with no server.
		if (cfg.send_enable) {
			int send_stat = send_buffer_over_ip(buffer, &tosend, cfg);
			//printf("Buffer return: %d\n", send_stat);
		}

		free(buffer);
	}
	//	std::cout << "IO Thread" << " completed " << count << std::endl;
}

void preview_update_thread(const Config* cfg_ptr, std::queue<void*>* buffer, std::mutex* mutex, int camera_id, int* killsignal) {
	const Config& cfg = *cfg_ptr;

	const size_t scaled_image_size = (size_t)cfg.panel_width * cfg.panel_height;
	uint8_t* scaled_image = (uint8_t*)malloc(scaled_image_size);
	uint16_t* frame = (uint16_t*)malloc(cfg.frame_bytes);
	if (scaled_image == NULL || frame == NULL) return;

	while (!(*killsignal)) {
		mutex->lock();
		memcpy(frame, buffer->front(), cfg.frame_bytes);
		mutex->unlock();

		// rescale
		memset(scaled_image, 0, scaled_image_size);
		uint16_t min_val = 65535, max_val = 0;
		const size_t pixel_count = (size_t)cfg.frame_width * cfg.frame_height;
		for (size_t i = 0; i < pixel_count; i++) {
			if (frame[i] < min_val) min_val = frame[i];
			if (frame[i] > max_val) max_val = frame[i];
		}

		// The frame can have one value only. This happens before the first
		// frame arrives. Then the division below is a division by zero.
		const double range = (max_val > min_val) ? (double)(max_val - min_val) : 0.0;

		for (uint32_t i = 0; i < cfg.panel_height; i++) {
			for (uint32_t j = 0; j < cfg.panel_width; j++) {
				double sum = 0;
				for (uint32_t ii = 0; ii < cfg.preview_scale; ii++) {
					for (uint32_t jj = 0; jj < cfg.preview_scale; jj++) {
						size_t offset_src = (size_t)((i * cfg.preview_scale) + ii) * cfg.frame_width;
						offset_src += ((j * cfg.preview_scale) + jj);
						sum += frame[offset_src];
					}
				}

				sum /= cfg.preview_scale; // scale x
				sum /= cfg.preview_scale; // scale y

				// rescale to 8 bit range...
				if (range > 0) {
					sum -= min_val;      // subtract lowest val
					sum /= range;        // divide by the range
					// sum should now be [0,1]
					sum *= 255;          // Maximum value of a uint8_t
				}
				else {
					sum = 0;
				}

				if (sum < 0) sum = 0;
				if (sum > 255) sum = 255;

				const uint8_t new_val = (uint8_t)sum;

				scaled_image[((size_t)i * cfg.panel_width) + j] = new_val;
			}
		}

		// Implement frame offset and flip?

		live_preview_mutex.lock();
		camera_min_vals[camera_id] = min_val;
		camera_max_vals[camera_id] = max_val;
		for (uint32_t i = 0; i < cfg.panel_height; i++) {
			// One row of the preview area has preview_stride bytes. A Windows
			// bitmap of 24 bits needs a row of a multiple of 4 bytes.
			RGB_Tuple* row = (RGB_Tuple*)(preview_buffer + (i * cfg.preview_stride));
			for (uint32_t j = 0; j < cfg.panel_width; j++) {
				const uint8_t value = scaled_image[((size_t)i * cfg.panel_width) + j];

				// Put the value in the three channels. This changes the black and
				// white data to RGB data.
				RGB_Tuple* pixel = row + ((size_t)camera_id * cfg.panel_width) + j;
				pixel->blue = value;
				pixel->green = value;
				pixel->red = value;
			}
		}
		live_preview_mutex.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	free(scaled_image);
	free(frame);
}

void camera_thread_main(const Config* cfg_ptr, FrameSource* source, BOOL* ready, uint8_t id, uint32_t burst_id, uint32_t expt_id) {
	const Config& cfg = *cfg_ptr;

	setThreadPriority(31);
	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	std::cout << std::setprecision(5);

	int kill_signal = FALSE;

	// The preview slot holds the newest frame only. The IO threads write it and
	// the preview thread reads it.
	std::queue<void*> preview_slot = {};
	std::mutex preview_mutex;
	void* preview_frame = malloc(cfg.frame_bytes);
	memset(preview_frame, 0, cfg.frame_bytes);
	preview_slot.push(preview_frame);
	std::thread preview_thread = std::thread(preview_update_thread, cfg_ptr, &preview_slot, &preview_mutex, id, &kill_signal);

	std::queue<io_tuple> io_buffer = {};
	std::vector<std::thread> io_threads;
	std::mutex io_mutex;

	for (uint32_t i = 0; i < cfg.io_threads; i++) {
		io_threads.push_back(
			std::thread(io_thread_loop, cfg_ptr, &io_buffer, &io_mutex, &kill_signal, &preview_mutex, &preview_slot)
		);
	}

	if (source->start()) {
		*ready = true;

		uint32_t dropped = 0;
		for (uint32_t i = 1; true; i++) { // count frames processed
			void* buffer = malloc(cfg.frame_bytes);
			if (buffer == NULL) {
				cout_mutex.lock();
				printf("[%d] The application cannot get the memory for a frame.\n", (int)id);
				cout_mutex.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			if (!source->next_frame(i, buffer)) {
				free(buffer);
				break;
			}

			sendme_timecodet timecode = get_current_timecode(); // Should get from camera 

			io_mutex.lock();
			// The queue has no limit of its own. Each frame in it is one full
			// frame of memory. If the network is slower than the camera, the
			// queue grows until the PC has no memory. Thus the queue has a
			// limit. The value 0 gives no limit.
			if (cfg.max_queue_depth > 0 && io_buffer.size() >= cfg.max_queue_depth) {
				io_mutex.unlock();
				free(buffer);
				dropped++;
				if (dropped == 1 || dropped % 100 == 0) {
					cout_mutex.lock();
					printf("[%d] THE QUEUE IS FULL. The application released %u frames.\n",
						(int)id, dropped);
					cout_mutex.unlock();
				}
				continue;
			}
			io_buffer.push({ i, id, burst_id, expt_id, timecode, (sendme_int)cfg.frame_bytes, buffer });
			io_mutex.unlock();
		}
	}
	else {
		cout_mutex.lock();
		printf("[%d] The frame source did not start.\n", (int)id);
		cout_mutex.unlock();
	}

	// The loop above stops only if the source has no more frames. Stop the
	// other threads of this camera before this function releases their memory.
	source->stop();
	kill_signal = TRUE;
	for (size_t i = 0; i < io_threads.size(); i++) io_threads[i].join();
	preview_thread.join();
	free(preview_frame);
}

int init_api_and_cameras(const Config& cfg, HDCAM* hdcams, DCAMWAIT_OPEN* waitopens, HDCAMWAIT* hwaits) {
	DCAMERR err;

	// Initialize DCAM-API ver 4.0
	DCAMAPI_INIT apiinit;
	memset(&apiinit, 0, sizeof(apiinit));
	apiinit.size = sizeof(apiinit);

	err = dcamapi_init(&apiinit);
	if (failed(err)) {
		dcamcon_show_dcamerr(NULL, err, "dcamapi_init()");
		return -1;
	}

	if ((uint32_t)apiinit.iDeviceCount < cfg.camera_count) {
		std::cout << "Wrong number of cameras detected!! The settings need "
			<< cfg.camera_count << ". The PC has " << apiinit.iDeviceCount
			<< "." << std::endl;
		return -1;
	}

	for (uint32_t i = 0; i < cfg.camera_count; i++) {
		hdcams[i] = get_camera_by_id((int32)i);

		// Fail if not loaded
		if (hdcams[i] == NULL) {
			std::cout << "One or more cameras failed to load (!!!) [i=" << i << "]" << std::endl;
			return -2;
		}

		// Print camera details for debugging
		std::cout << "CAMERA " << (int)i << " INFORMATION:" << std::endl;
		dcamcon_show_dcamdev_info(hdcams[i]);
		std::cout << std::endl;

		// Hack camera settings :(
		for (uint8_t j = 0; j < 5; j++) {
			assignSettings(hdcams[i], cfg);
		}

		memset(&waitopens[i], 0, sizeof(waitopens[i]));
		waitopens[i].size = sizeof(waitopens[i]);
		waitopens[i].hdcam = hdcams[i];

		err = dcamwait_open(&waitopens[i]);
		if (failed(err)) {
			dcamcon_show_dcamerr(hdcams[i], err, "dcamwait_open()");
			return -3;
		}

		hwaits[i] = waitopens[i].hwait;

		err = dcambuf_alloc(hdcams[i], (int32)cfg.dcam_buffers);
		if (failed(err))
		{
			dcamcon_show_dcamerr(hdcams[i], err, "dcambuf_alloc()");
			return -4;
		}
	}

	return apiinit.iDeviceCount;
}

void kill_api_and_cameras(const Config& cfg, HDCAM* hdcams, HDCAMWAIT* hwaits) {
	for (uint32_t i = 0; i < cfg.camera_count; i++) {
		//dcamcap_stop(hdcams[i]);
		dcambuf_release(hdcams[i]);
		dcamwait_close(hwaits[i]);
		dcamdev_close(hdcams[i]);
	}

	// finalize DCAM-API
	dcamapi_uninit();
}

// Window procedure
std::deque<float> frame_rate_buffer = {};
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	const Config& cfg = g_config;
	const int panel_w = (int)cfg.panel_width;
	const int panel_h = (int)cfg.panel_height;
	const int image_w = panel_w * (int)cfg.camera_count;

	switch (msg)
	{
	case WM_PAINT:
	{
		auto start = std::chrono::high_resolution_clock::now();

		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		// Create a memory DC and bitmap
		HDC memDC = CreateCompatibleDC(hdc);
		HBITMAP hBitmap = CreateCompatibleBitmap(hdc, image_w, panel_h);

		// Select the bitmap into the memory DC
		HGDIOBJ hOldBitmap = SelectObject(memDC, hBitmap);

		// Set the bitmap bits from the memory buffer
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = image_w;
		bmi.bmiHeader.biHeight = -panel_h;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		SetTextColor(hdc, RGB(0, 0, 0));
		SetBkColor(hdc, RGB(255, 255, 255));
		live_preview_mutex.lock();
		SetDIBits(hdc, hBitmap, 0, panel_h, preview_buffer, &bmi, DIB_RGB_COLORS);
		for (uint32_t i = 0; i < cfg.camera_count; i++) {
			std::wstringstream stringbuilder;
			stringbuilder << camera_min_vals[i] << " -> " << camera_max_vals[i] << "      ";
			std::wstring out = stringbuilder.str();
			TextOut(hdc, (panel_w * (int)i) + 20, panel_h + 10, out.c_str(), (int)out.size());
		}
		live_preview_mutex.unlock();

		SetTextColor(hdc, RGB(255, 0, 0));
		SetBkColor(hdc, RGB(255, 255, 255));

		float avg_frame_time = accumulate(frame_rate_buffer.begin(), frame_rate_buffer.end(), 0.0);
		avg_frame_time /= frame_rate_buffer.size();

		{
			std::wstringstream stringbuilder;
			if (cfg.demo_mode)
				stringbuilder << "DEMO MODE - NOT REAL DATA    ";
			stringbuilder << "GUI Draw Time:  " << std::fixed << std::setprecision(2);
			if (frame_rate_buffer.size() == 0) {
				stringbuilder << "???";
			}
			else {
				stringbuilder << avg_frame_time;
			}
			stringbuilder << " ms    ";
			std::wstring frame_rate_string = stringbuilder.str();

			SetTextAlign(hdc, TA_LEFT);
			TextOut(hdc, 20, panel_h + 10 + 25, frame_rate_string.c_str(), (int)frame_rate_string.size());
		}


		// Blit the memory DC to the window DC
		BitBlt(hdc, 0, 0, image_w, panel_h, memDC, 0, 0, SRCCOPY);

		// Clean up
		SelectObject(memDC, hOldBitmap);
		DeleteObject(hBitmap);
		DeleteDC(memDC);
		EndPaint(hwnd, &ps);

		auto stop = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
		float frame_time = duration.count() / 1000.0;

		frame_rate_buffer.push_front(frame_time);
		while (frame_rate_buffer.size() > 50)
			frame_rate_buffer.pop_back();

		InvalidateRect(hwnd, NULL, FALSE);

		break;
	}

	case WM_TIMER:
	{
		//InvalidateRect(hwnd, NULL, FALSE);
		break;
	}

	case WM_DESTROY:
	{
		// Exit the application
		PostQuitMessage(0);
		break;
	}

	default:
	{
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	}

	return 0;
}

/// Makes the text of the title of the window. The title shows the demo mode and
/// the address of the storage host. Thus a person cannot confuse a demo run
/// with a real run.
static std::wstring make_window_title(const Config& cfg) {
	std::wstringstream out;
	if (cfg.demo_mode) out << L"DEMO MODE - NOT REAL DATA - ";
	out << L"Camera Live Preview  [";
	if (cfg.send_enable) {
		out << convert_narrow_to_wide_string(cfg.server_host)
			<< L":" << cfg.server_port;
	}
	else {
		out << L"the application sends no data";
	}
	out << L"]";
	return out.str();
}

// Entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	// Redirect stdout to a console window
	AllocConsole();
	FILE* pConsoleStream;
	freopen_s(&pConsoleStream, "CONOUT$", "w", stdout);
	printf("Logging started...\n");

	// Read the settings. This must happen before any thread starts. See
	// config.h. If a value is not correct, load_config stops the application.
	g_config = load_config();
	const Config& cfg = g_config;
	show_config(cfg);

	if (cfg.demo_mode) {
		printf("****************************************************\n");
		printf("*                                                  *\n");
		printf("*   DEMO MODE. THIS IS NOT MICROSCOPE DATA.        *\n");
		printf("*   The application makes its own frames.          *\n");
		printf("*   Each frame has expid = %-8u in its header. *\n", cfg.demo_expid);
		printf("*                                                  *\n");
		printf("****************************************************\n\n");
		fflush(stdout);
	}

	// Open HDCAM pointers
	HDCAM hdcams[MAX_CAMERAS];
	DCAMWAIT_OPEN waitopens[MAX_CAMERAS];
	HDCAMWAIT hwaits[MAX_CAMERAS];
	memset(hdcams, 0, sizeof(hdcams));
	memset(hwaits, 0, sizeof(hwaits));

	if (!cfg.demo_mode) {
		// This is the first DCAM call. Windows loads dcamapi.dll here.
		if (init_api_and_cameras(cfg, hdcams, waitopens, hwaits) < 0) {
			printf("The application cannot open the cameras. It stops.\n");
			fflush(stdout);
			MessageBoxA(NULL,
				"The application cannot open the cameras.\n\n"
				"The console window gives the message of DCAM-API.\n\n"
				"To test the application without cameras, set demo_mode = 1.",
				"cameraman_windows", MB_ICONERROR | MB_OK);
			return 1;
		}
	}

	// Allocate and clear buffers. This must happen BEFORE the threads start,
	// because a preview thread writes into this memory.
	preview_buffer = (uint8_t*)malloc(cfg.preview_bytes);
	if (preview_buffer == NULL) {
		MessageBoxA(NULL, "The application cannot get the memory for the preview.",
			"cameraman_windows", MB_ICONERROR | MB_OK);
		return 1;
	}
	memset(preview_buffer, 0, cfg.preview_bytes);
	memset(camera_min_vals, 0, sizeof(camera_min_vals));
	memset(camera_max_vals, 0, sizeof(camera_max_vals));

	// Launch capture threads
	BOOL ready[MAX_CAMERAS];
	FrameSource* sources[MAX_CAMERAS];
	uint32_t burst_id = 0;
	// In demo mode each frame has a mark in the header. The value burstid and
	// the value expid are 0 in a real run. The storage host writes expid in its
	// log. Thus you can find the demo frames.
	uint32_t expt_id = cfg.demo_mode ? cfg.demo_expid : 0;
	std::thread threads[MAX_CAMERAS];
	for (uint32_t i = 0; i < cfg.camera_count; i++) {
		ready[i] = false;
		sources[i] = cfg.demo_mode
			? make_demo_source(cfg, (uint8_t)i)
			: make_dcam_source(cfg, (uint8_t)i, hdcams[i]);
		threads[i] = std::thread(camera_thread_main, &g_config, sources[i], &ready[i], (uint8_t)i, burst_id, expt_id);
	}

	// Register the window class
	WNDCLASSEX wc = {
		sizeof(WNDCLASSEX),
		CS_HREDRAW | CS_VREDRAW,
		WndProc,
		0,
		0,
		hInstance,
		LoadIcon(NULL, IDI_APPLICATION),
		LoadCursor(NULL, IDC_ARROW),
		NULL,
		NULL,
		TEXT("myWindowClass"),
		NULL
	};
	wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 255));
	if (!RegisterClassEx(&wc))
	{
		MessageBox(NULL, TEXT("Window Registration Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

	// The client area holds one panel for each camera and two lines of text
	// below them. AdjustWindowRect adds the size of the border and the title.
	RECT window_rect = {
		0, 0,
		(LONG)(cfg.panel_width * cfg.camera_count),
		(LONG)(cfg.panel_height + 80)
	};
	AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

	const std::wstring title = make_window_title(cfg);

	// Create the window
	HWND hwnd = CreateWindowEx(
		0,
		TEXT("myWindowClass"),
		title.c_str(),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		window_rect.right - window_rect.left,
		window_rect.bottom - window_rect.top,
		NULL,
		NULL,
		hInstance,
		NULL
	);
	if (!hwnd)
	{
		MessageBox(NULL, TEXT("Window Creation Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

	// Show the window
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	MSG msg;
	while ((GetMessage(&msg, nullptr, 0, 0))) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	printf("\nThe preview window closed. The capture threads continue.\n");
	printf("To stop the application, use Ctrl-C in this window.\n");
	fflush(stdout);

	while (1) std::this_thread::sleep_for(std::chrono::seconds(1));

	kill_api_and_cameras(cfg, hdcams, hwaits);

	return 0;
}
