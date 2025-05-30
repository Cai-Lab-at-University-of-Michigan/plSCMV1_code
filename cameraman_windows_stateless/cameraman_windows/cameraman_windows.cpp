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
///// ----------------------------

#include "util.h"
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

#define CAMERA_NUMS 3
#define IO_THREAD_CONCURRENCY 5
#define FRAME_BUFFER_COUNT 1000

// Parameters used for cameraman server (hardcoded right now)
#define PORT 8080
#define SERVERIP "sndif.cai-lab.org"
#define SEND_CHUNK 4096*4
//////

// Parameters for camera timing
#define H_INTERVAL (0.000004868)
// 0.000004868 / 2 = 0.0000024338
#define TRIGGER_INTERVAL (0.002)
#define EXPOSURE_TIME 0.000017632
//#define EXPOSURE_TIME 0.0001
#define HSYNC 150
//////

// Parameters for each frame
#define FRAME_WIDTH 2304
#define FRAME_HEIGHT 2304
#define FRAME_BYTES_PER_PX 2
#define FRAME_WAIT_INTERVAL 1000
//////


// Parameters for live preview
#define PREVIEW_SCALE_FACTOR 4
uint8_t* preview_buffer;
size_t preview_buffer_size = 3 * FRAME_WIDTH * FRAME_HEIGHT * 3 / PREVIEW_SCALE_FACTOR / PREVIEW_SCALE_FACTOR;
uint16_t camera_min_vals[CAMERA_NUMS], camera_max_vals[CAMERA_NUMS];
///////

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

void assignSettings(HDCAM hdcam) {
	std::cout << "Calling camera settings: " << hdcam << std::endl;
	dcamprop_setvalue(hdcam, DCAM_IDPROP_SENSORMODE, DCAMPROP_SENSORMODE__PROGRESSIVE); // set progressive mode (=2)
	dcamprop_setvalue(hdcam, DCAM_IDPROP_READOUTSPEED, 3); // set fast mode (=3)
	dcamprop_setvalue(hdcam, DCAM_IDPROP_SENSORCOOLER, DCAMPROP_SENSORCOOLER__ON); // sensor cooler on
	dcamprop_setvalue(hdcam, DCAM_IDPROP_BINNING, DCAMPROP_BINNING__1); // binning for test

	dcamprop_setvalue(hdcam, DCAM_IDPROP_EXPOSURETIME, EXPOSURE_TIME); // force exposure time
	dcamprop_setvalue(hdcam, DCAM_IDPROP_INTERNAL_LINEINTERVAL, H_INTERVAL);
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

	dcamprop_setvalue(hdcam, DCAM_IDPROP_OUTPUTTRIGGER_PREHSYNCCOUNT, HSYNC); // PRESYNC
	dcamprop_setvalue(hdcam, DCAM_IDPROP_OUTPUTTRIGGER_DELAY, 0); // Delay output trigger

	// -- Trigger 0 --
	dcamprop_setvalue(hdcam, 0x001c0110, DCAMPROP_OUTPUTTRIGGER_SOURCE__TRIGGER); // source
	dcamprop_setvalue(hdcam, 0x001c0120, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0130, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0150, TRIGGER_INTERVAL); // period
	dcamprop_setvalue(hdcam, 0x001c0160, DCAMPROP_OUTPUTTRIGGER_KIND__PROGRAMABLE); //kind

	// -- Trigger 1 -- 
	dcamprop_setvalue(hdcam, 0x001c0210, DCAMPROP_OUTPUTTRIGGER_SOURCE__TRIGGER); // source
	dcamprop_setvalue(hdcam, 0x001c0220, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0230, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0250, TRIGGER_INTERVAL); // period
	dcamprop_setvalue(hdcam, 0x001c0260, DCAMPROP_OUTPUTTRIGGER_KIND__PROGRAMABLE); //kind

	// -- Trigger 2 --
	dcamprop_setvalue(hdcam, 0x001c0310, DCAMPROP_OUTPUTTRIGGER_SOURCE__HSYNC); // source
	dcamprop_setvalue(hdcam, 0x001c0320, DCAMPROP_OUTPUTTRIGGER_POLARITY__POSITIVE); // polarity
	dcamprop_setvalue(hdcam, 0x001c0330, DCAMPROP_OUTPUTTRIGGER_ACTIVE__EDGE);  // edge mode
	dcamprop_setvalue(hdcam, 0x001c0350, H_INTERVAL / 2);  // 0.0000024338); // period
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

int send_buffer_over_ip(void* buffer, sendme* tosend, const char* server_ip, int connect_port) {
	WSADATA wsaData;
	struct addrinfo* address = NULL;
	struct addrinfo hints;

	size_t cbuffer_size = tosend->payload_size * 2;
	void* cbuffer = malloc(cbuffer_size);
	size_t csize = ZSTD_compress(cbuffer, cbuffer_size, buffer, tosend->payload_size, 1);

	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	// Resolve the server address and port
	iResult = getaddrinfo(server_ip, std::to_string(connect_port).c_str(), &hints, &address);
	if (iResult != 0) {
		printf("getaddrinfo failed with error: %d\n", iResult);
		WSACleanup();
		return -2;
	}

	// Create connection socket
	SOCKET ConnectSocket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
	if (ConnectSocket == INVALID_SOCKET) {
		printf("socket failed with error: %ld\n", WSAGetLastError());
		WSACleanup();
		return -3;
	}

	// Connect to server.
	if (connect(ConnectSocket, address->ai_addr, (int)address->ai_addrlen) == SOCKET_ERROR) {
		closesocket(ConnectSocket);
		printf("Unable to connect to server!\n");
		WSACleanup();
		return -4;
	}

	// Send an initial buffer
	iResult = send(ConnectSocket, (const char*)tosend, (int)sizeof(sendme), 0);
	if (iResult == SOCKET_ERROR) {
		printf("Header send failed with error: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		WSACleanup();
		return -5;
	}
	//printf("Bytes Sent: %ld\n", iResult);

	iResult = send(ConnectSocket, (const char*)cbuffer, (int)csize, 0);
	if (iResult == SOCKET_ERROR) {
		printf("Buffer send failed with error: %d\n", WSAGetLastError());
		printf("\tBuffer size: %zu / %zu\n", csize, cbuffer_size);
		closesocket(ConnectSocket);
		WSACleanup();
		return -6;
	}
	//printf("Bytes Sent: %ld\n", iResult);

	// shutdown the connection since no more data will be sent
	iResult = shutdown(ConnectSocket, SD_SEND);
	if (iResult == SOCKET_ERROR) {
		printf("shutdown failed with error: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		WSACleanup();
		return -7;
	}

	// cleanup
	closesocket(ConnectSocket);
	freeaddrinfo(address);
	free(cbuffer);
	WSACleanup();

	return 1;
}

void io_thread_loop(std::queue<io_tuple>* io_buffer, std::mutex* io_mutex, int* killsignal, std::mutex* preview_mutex, std::queue<void*>* preview_buffer) {
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
		//void* for_preview = malloc(FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_PX);
		//memcpy(for_preview, buffer, FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_PX);
		preview_mutex->lock();
		//preview_buffer->push(for_preview);
		memcpy(preview_buffer->front(), buffer, FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_PX);
		preview_mutex->unlock();

		// rescale buffer
		if (false)
		{
			tosend.payload_size /= 2;
			uint16_t* buffer_cast = (uint16_t*)buffer;
			uint8_t* buffer_processed = (uint8_t*)malloc(tosend.payload_size);
			register float tmp = 0;
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

		// Write to IP buffer
		int send_stat = send_buffer_over_ip(buffer, &tosend, SERVERIP, PORT);
		//printf("Buffer return: %d\n", send_stat);

		free(buffer);
	}
	//	std::cout << "IO Thread" << " completed " << count << std::endl;
}

void preview_update_thread(std::queue<void*>* buffer, std::mutex* mutex, int camera_id, int* killsignal) {
	const size_t scaled_image_size = FRAME_HEIGHT * FRAME_WIDTH / PREVIEW_SCALE_FACTOR / PREVIEW_SCALE_FACTOR;
	uint8_t* scaled_image = (uint8_t*)malloc(scaled_image_size);
	uint16_t* frame = (uint16_t*)malloc(FRAME_HEIGHT * FRAME_WIDTH * sizeof(uint16_t));

	while (!(*killsignal)) {
		mutex->lock();
		//while (buffer->size() > 1) {
		//	free(buffer->front());
		//	buffer->pop();
		//}
		//if (buffer->size() > 0) { // read all existing frames
		//	frame = (uint16_t*)buffer->front();
		//	buffer->pop();
		//	//std::cout << "read a frame... " << frame << ' ' << frame[0] << std::endl;
		//}
		memcpy(frame, buffer->front(), FRAME_HEIGHT * FRAME_WIDTH * sizeof(uint16_t));
		mutex->unlock();

		if (frame == 0) { // no frame available
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

		// rescale
		memset(scaled_image, 0, scaled_image_size);
		double min = DBL_MAX, max = DBL_MIN;
		for (size_t i = 0; i < FRAME_HEIGHT * FRAME_WIDTH; i++) {
			min = min(min, frame[i]);
			max = max(max, frame[i]);
		}

		for (size_t i = 0; i < FRAME_HEIGHT / PREVIEW_SCALE_FACTOR; i++) {
			for (size_t j = 0; j < FRAME_WIDTH / PREVIEW_SCALE_FACTOR; j++) {
				register double sum = 0;
				for (size_t ii = 0; ii < PREVIEW_SCALE_FACTOR; ii++) {
					for (size_t jj = 0; jj < PREVIEW_SCALE_FACTOR; jj++) {
						size_t offset_src = ((i * PREVIEW_SCALE_FACTOR) + ii) * FRAME_WIDTH;
						offset_src += ((j * PREVIEW_SCALE_FACTOR) + jj);
						sum += frame[offset_src];
					}
				}

				sum /= PREVIEW_SCALE_FACTOR; // scale x
				sum /= PREVIEW_SCALE_FACTOR; // scale y

				// rescale to 8 bit range...
				// sum = sqrt(sum);
				sum -= min; // subtract lowest val
				sum /= (max - min); // divide by largest val
				// sum should now be [0,1]
				sum *= 255; // Maximum value of a uint8_t

				if (sum < 0) sum = 0;
				if (sum > 255) sum = 255;

				const uint8_t new_val = (uint8_t)sum;

				scaled_image[(i * (FRAME_WIDTH / PREVIEW_SCALE_FACTOR)) + j] = new_val;
			}
		}

		// Implement frame offset and flip?

		RGB_Tuple* preview_buffer_cast = (RGB_Tuple*)preview_buffer;
		live_preview_mutex.lock();
		camera_min_vals[camera_id] = min;
		camera_max_vals[camera_id] = max;
		for (size_t i = 0; i < FRAME_HEIGHT / PREVIEW_SCALE_FACTOR; i++) {
			for (size_t j = 0; j < FRAME_WIDTH / PREVIEW_SCALE_FACTOR; j++) {
				const size_t offset_src = (i * FRAME_WIDTH / PREVIEW_SCALE_FACTOR) + j;
				const size_t offset_dest = (i * 3 * FRAME_WIDTH / PREVIEW_SCALE_FACTOR) + (camera_id * FRAME_WIDTH / PREVIEW_SCALE_FACTOR) + j;

				// Map all three channels to the buffer (i.e. b/w -> rgb)
				preview_buffer_cast[offset_dest].blue = scaled_image[offset_src];
				preview_buffer_cast[offset_dest].green = scaled_image[offset_src];
				preview_buffer_cast[offset_dest].red = scaled_image[offset_src];
			}
		}
		live_preview_mutex.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

void camera_thread_main(BOOL* trigger, BOOL* ready, HDCAM hdcam, HDCAMWAIT hwait, uint8_t id, uint32_t burst_id, uint32_t expt_id) {
	setThreadPriority(31);
	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	std::cout << std::setprecision(5);

	// wait start param
	DCAMERR err;
	DCAMWAIT_START waitstart{
		.size = sizeof(DCAMWAIT_START),
		.eventhappened = 0,
		.eventmask = DCAMWAIT_CAPEVENT_FRAMEREADY,
		.timeout = FRAME_WAIT_INTERVAL
	};
	DCAMBUF_FRAME bufframe{
		.size = sizeof(DCAMBUF_FRAME),
		.iKind = 0,
		.option = 0,
		.iFrame = -1,
		.buf = NULL,
		.rowbytes = FRAME_WIDTH * FRAME_BYTES_PER_PX,
		.type = DCAM_PIXELTYPE_NONE,
		.width = FRAME_WIDTH,
		.height = FRAME_HEIGHT,
		.left = 0,
		.top = 0,
		.timestamp = 0,
		.framestamp = 0,
		.camerastamp = 0
	};

	int kill_signal = FALSE;

	std::queue<void*> preview_buffer = {};
	std::mutex preview_mutex;
	void* preview_buffer_frame = malloc(FRAME_HEIGHT * FRAME_WIDTH * sizeof(uint16_t));
	memset(preview_buffer_frame, 0, FRAME_HEIGHT * FRAME_WIDTH * sizeof(uint16_t));
	preview_buffer.push(preview_buffer_frame); // Load one frame into buffer to be liveview
	std::thread preview_thread = std::thread(preview_update_thread, &preview_buffer, &preview_mutex, id, &kill_signal);

	std::queue<io_tuple> io_buffer = {};
	std::vector<std::thread> io_threads;
	std::mutex io_mutex;

	for (int i = 0; i < IO_THREAD_CONCURRENCY; i++) {
		io_threads.push_back(
			std::thread(io_thread_loop, &io_buffer, &io_mutex, &kill_signal, &preview_mutex, &preview_buffer)
		);
	}

	err = dcamcap_start(hdcam, DCAMCAP_START_SEQUENCE);
	if (failed(err))
	{ /// TODO add better error handling here
		dcamcon_show_dcamerr(hdcam, err, "dcamcap_start()");
	}

	*ready = true;

	//auto overall_start = std::chrono::high_resolution_clock::now();
	DCAMCAP_TRANSFERINFO captransferinfo;
	for (uint32_t i = 1; true; i++) { // count frames processed
		memset(&captransferinfo, 0, sizeof(captransferinfo));
		captransferinfo.size = sizeof(captransferinfo);
		captransferinfo.iKind = DCAMCAP_TRANSFERKIND_FRAME;

		//	do { // Delay until a new frame is ready
		//		err = dcamwait_start(hwait, &waitstart);
		//	} while (failed(err)); // Loop until ready

		do {
			err = dcamcap_transferinfo(hdcam, &captransferinfo);
			if (failed(err))
			{
				dcamcon_show_dcamerr(hdcam, err, "dcamcap_transferinfo()");
				//return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (captransferinfo.nFrameCount < i);

		cout_mutex.lock();
		printf("[%d][#%09d] Got %d frames, last %d\n", id, i, captransferinfo.nFrameCount, captransferinfo.nNewestFrameIndex);
		printf("[%d] GAP = %d\n", id, ((int)captransferinfo.nFrameCount) - ((int)i));
		cout_mutex.unlock();

		bufframe.buf = malloc(FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_PX);
		bufframe.iFrame = (i - 1) % FRAME_BUFFER_COUNT;

		// Invalid parameter copyframe
		do {
			err = dcambuf_copyframe(hdcam, &bufframe);
			if (failed(err) && err != DCAMERR_BUSY) dcamcon_show_dcamerr(hdcam, err, "dcambuf_copyframe()");
		} while (err == DCAMERR_BUSY);
		sendme_timecodet timecode = get_current_timecode(); // Should get from camera 

		io_mutex.lock();
		io_buffer.push({ i, id, burst_id, expt_id, timecode, FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_PX, bufframe.buf });
		io_mutex.unlock();
	}

	//double overall_elapsed_time = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - overall_start).count();

	/*
	dcamcap_stop(hdcam);
	kill_signal = TRUE; // Tell io threads to die (should happen in ~1ms +/- current write activity)
	for (int i = 0; i < io_threads.size(); i++) {
		io_threads[i].join(); // TODO maybe this isn't needed (could allow async flushing in bg)
	}
	preview_thread.join();
	*/
}

int init_api_and_cameras(HDCAM* hdcams, DCAMWAIT_OPEN* waitopens, HDCAMWAIT* hwaits) {
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

	if (apiinit.iDeviceCount < CAMERA_NUMS) {
		std::cout << "Wrong number of cameras detected!!" << std::endl;
		return -1;
	}

	for (uint8_t i = 0; i < CAMERA_NUMS; i++) {
		hdcams[i] = get_camera_by_id(i);

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
			assignSettings(hdcams[i]);
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

		err = dcambuf_alloc(hdcams[i], FRAME_BUFFER_COUNT);
		if (failed(err))
		{
			dcamcon_show_dcamerr(hdcams[i], err, "dcambuf_alloc()");
			return -4;
		}
	}

	return apiinit.iDeviceCount;
}

void kill_api_and_cameras(HDCAM* hdcams, HDCAMWAIT* hwaits) {
	for (uint8_t i = 0; i < CAMERA_NUMS; i++) {
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
	switch (msg)
	{
	case WM_PAINT:
	{
		auto start = std::chrono::high_resolution_clock::now();

		HBRUSH hBrush;
		RECT rect;
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		// Create a memory DC and bitmap
		HDC memDC = CreateCompatibleDC(hdc);
		HBITMAP hBitmap = CreateCompatibleBitmap(hdc, FRAME_WIDTH, FRAME_HEIGHT);

		// Select the bitmap into the memory DC
		HGDIOBJ hOldBitmap = SelectObject(memDC, hBitmap);

		// Set the bitmap bits from the memory buffer
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = 3 * FRAME_WIDTH / PREVIEW_SCALE_FACTOR;
		bmi.bmiHeader.biHeight = -FRAME_HEIGHT / PREVIEW_SCALE_FACTOR;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		SetTextColor(hdc, RGB(0, 0, 0));
		SetBkColor(hdc, RGB(255, 255, 255));
		live_preview_mutex.lock();
		SetDIBits(hdc, hBitmap, 0, FRAME_HEIGHT / PREVIEW_SCALE_FACTOR, preview_buffer, &bmi, DIB_RGB_COLORS);
		for (size_t i = 0; i < CAMERA_NUMS; i++) {
			std::wstringstream stringbuilder;
			stringbuilder << camera_min_vals[i] << " -> " << camera_max_vals[i] << "      ";
			std::wstring out = stringbuilder.str();
			TextOut(hdc, ((2304 / 4) * i) + 20, (2304 / 4) + 10, out.c_str(), out.size());
		}
		live_preview_mutex.unlock();

		//        rect = { 0, 0, 100, 100 };
		//        hBrush = CreateSolidBrush(RGB(255, 0, 0));
		//        FillRect(hdc, &rect, hBrush);
		//        DeleteObject(hBrush);

		SetTextColor(hdc, RGB(255, 0, 0));
		SetBkColor(hdc, RGB(255, 255, 255));

		float avg_frame_time = accumulate(frame_rate_buffer.begin(), frame_rate_buffer.end(), 0.0);
		avg_frame_time /= frame_rate_buffer.size();

		{
			std::wstringstream stringbuilder;
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
			TextOut(hdc, 20, (2304 / 4) + 10 + 25, frame_rate_string.c_str(), frame_rate_string.size());
		}


		// Blit the memory DC to the window DC
		BitBlt(hdc, 0, 0, 3 * FRAME_WIDTH / PREVIEW_SCALE_FACTOR, FRAME_HEIGHT / PREVIEW_SCALE_FACTOR, memDC, 0, 0, SRCCOPY);
		//StretchBlt(hdc, 0, 0, 2304 / 4, 2304 / 4, memDC, 0, 0, 2304/2, 2304/2, SRCCOPY);

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

		//std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

// Entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	// Redirect stdout to a console window
	AllocConsole();
	FILE* pConsoleStream;
	freopen_s(&pConsoleStream, "CONOUT$", "w", stdout);
	printf("Logging started...\n");

	// Open HDCAM pointers
	HDCAM hdcams[CAMERA_NUMS];
	DCAMWAIT_OPEN waitopens[CAMERA_NUMS];
	HDCAMWAIT hwaits[CAMERA_NUMS];

	int32 nDevice = init_api_and_cameras(hdcams, waitopens, hwaits); // apiinit.iDeviceCount;

	// Launch capture threads
	BOOL trigger = false, ready[CAMERA_NUMS];
	uint32_t expt_id = 0, burst_id = 0;
	std::thread threads[CAMERA_NUMS];
	for (size_t i = 0; i < CAMERA_NUMS; i++) {
		// TODO add capture number to this
		ready[i] = false;
		threads[i] = std::thread(camera_thread_main, &trigger, &ready[i], hdcams[i], hwaits[i], i, burst_id, expt_id);
	}

	//printf("Waiting for camera ready signals...\n");
	//for (size_t i = 0; i < CAMERA_NUMS; i++)
	//	while (!ready[i])
	//		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	//printf("Cameras ready.\n");

	// Allocate and clear buffers
	preview_buffer = (uint8_t*)malloc(preview_buffer_size);
	memset(preview_buffer, 0, preview_buffer_size);
	memset(camera_min_vals, 0, CAMERA_NUMS * sizeof(uint16_t));
	memset(camera_max_vals, 0, CAMERA_NUMS * sizeof(uint16_t));

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

	// Create the window
	HWND hwnd = CreateWindowEx(
		0,
		TEXT("myWindowClass"),
		TEXT("Camera Live Preview"),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		(2304 / 4) * 3,
		(600) + 200,
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

	while (1) std::this_thread::sleep_for(std::chrono::seconds(1));

	kill_api_and_cameras(hdcams, hwaits);

	return 0;
}