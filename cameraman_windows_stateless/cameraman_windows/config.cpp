/*
 * config.cpp: reads the settings of the capture application.
 *
 * See config.h for the four layers and the rules.
 */

#include "config.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// MSVC gives the command line of a window application in __argc and __argv.
// These two names are macros of the C runtime. Do not declare them again.
#include <stdlib.h>

/// The default values. The application operates correctly with no
/// configuration. These values agree with the microscope.
namespace defaults {
	// This is an example address. It is not a real host. Set server_host in
	// cameraman.ini, in PLSCM_SERVER_HOST, or with --server_host= .
	static const char* SERVER_HOST = "sndif.example.com";
	static const uint16_t SERVER_PORT = 8080;
	static const bool     SEND_ENABLE = true;
	static const int      ZSTD_LEVEL = 1;

	static const uint32_t CAMERA_COUNT = 3;
	static const uint32_t FRAME_WIDTH = 2304;
	static const uint32_t FRAME_HEIGHT = 2304;
	static const uint32_t FRAME_BYTES_PER_PX = 2;
	static const uint32_t IO_THREADS = 5;
	static const uint32_t DCAM_BUFFERS = 1000;
	static const uint32_t MAX_QUEUE_DEPTH = 64;

	static const double   H_INTERVAL = 0.000004868;
	static const double   EXPOSURE_TIME = 0.000017632;
	static const double   TRIGGER_INTERVAL = 0.002;
	static const uint32_t HSYNC = 150;

	static const uint32_t PREVIEW_SCALE = 4;

	static const bool     DEMO_MODE = false;
	static const uint32_t DEMO_INTERVAL_MS = 100;
	static const char* DEMO_PATTERN = "bars";
	static const char* DEMO_FILE = "";
	static const uint32_t DEMO_NOISE_BITS = 16;
	static const uint32_t DEMO_EXPID = 0xDE305;
}

/// The name of each setting. The file and the command line use this name. The
/// environment variable is PLSCM_ and this name in capital letters.
static const char* const KEY_NAMES[] = {
	"config",
	"server_host", "server_port", "send_enable", "zstd_level",
	"camera_count", "frame_width", "frame_height", "frame_bytes_per_px",
	"io_threads", "dcam_buffers", "max_queue_depth",
	"h_interval", "exposure_time", "trigger_interval", "hsync",
	"preview_scale",
	"demo_mode", "demo_interval_ms", "demo_pattern", "demo_file",
	"demo_noise_bits", "demo_expid",
};
static const size_t KEY_COUNT = sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]);

/// One value and the layer that gave it.
struct Entry {
	std::string value;
	std::string origin;
};
typedef std::map<std::string, Entry> Store;

/// The values of the last call of load_config. show_config reads this store.
static Store g_store;

// ----------------------------------------------------------------

/// Shows a message and stops the application. The application is a window
/// application. Thus a message on the console is not sufficient.
static void fail(const std::string& text) {
	printf("\nCONFIGURATION ERROR: %s\n", text.c_str());
	fflush(stdout);
	MessageBoxA(NULL, text.c_str(),
		"cameraman_windows: configuration error", MB_ICONERROR | MB_OK);
	exit(2);
}

/// Removes the space characters at the two ends of a string.
static std::string trim(const std::string& in) {
	const char* SPACE = " \t\r\n";
	const size_t first = in.find_first_not_of(SPACE);
	if (first == std::string::npos) return "";
	const size_t last = in.find_last_not_of(SPACE);
	return in.substr(first, last - first + 1);
}

static std::string to_upper(const std::string& in) {
	std::string out = in;
	for (size_t i = 0; i < out.size(); i++)
		out[i] = (char)toupper((unsigned char)out[i]);
	return out;
}

static bool is_known_key(const std::string& key) {
	for (size_t i = 0; i < KEY_COUNT; i++)
		if (key == KEY_NAMES[i]) return true;
	return false;
}

/// Gives the names of all the settings. A message of an error shows them.
static std::string all_key_names() {
	std::string out;
	for (size_t i = 0; i < KEY_COUNT; i++) {
		out += "  ";
		out += KEY_NAMES[i];
		out += "\n";
	}
	return out;
}

// ----------------------------------------------------------------
// The four layers

/// Layer 4, first part. Reads --<name>=<value> from the command line.
static void read_command_line(Store& store) {
	for (int i = 1; i < __argc; i++) {
		std::string arg = __argv[i];
		if (arg.rfind("--", 0) != 0) {
			fail("This item of the command line is not correct: " + arg +
				"\n\nThe format is --<name>=<value>.");
		}
		arg = arg.substr(2);
		const size_t eq = arg.find('=');
		if (eq == std::string::npos) {
			fail("This item of the command line has no value: --" + arg +
				"\n\nThe format is --<name>=<value>.");
		}
		const std::string key = trim(arg.substr(0, eq));
		const std::string value = trim(arg.substr(eq + 1));
		if (!is_known_key(key)) {
			fail("This name on the command line is not known: " + key +
				"\n\nThe names are:\n" + all_key_names());
		}
		Entry e = { value, "command line" };
		store[key] = e;
	}
}

/// Gives the value of an environment variable. Gives false if it is not set.
static bool env_get(const std::string& name, std::string& out) {
	const DWORD size = GetEnvironmentVariableA(name.c_str(), NULL, 0);
	if (size == 0) return false;             // Not set
	std::vector<char> buffer(size);
	const DWORD written =
		GetEnvironmentVariableA(name.c_str(), &buffer[0], size);
	if (written == 0 || written >= size) return false;
	out.assign(&buffer[0], written);
	return true;
}

/// Layer 3. Reads the PLSCM_ environment variables.
static void read_environment(Store& store) {
	for (size_t i = 0; i < KEY_COUNT; i++) {
		const std::string key = KEY_NAMES[i];
		const std::string name = "PLSCM_" + to_upper(key);
		std::string value;
		if (!env_get(name, value)) continue;
		Entry e = { trim(value), name };
		store[key] = e;
	}
}

/// Gives the directory of the executable file, with the last backslash.
static std::string executable_directory() {
	char path[MAX_PATH + 1];
	const DWORD size = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (size == 0 || size > MAX_PATH) return "";
	std::string out(path, size);
	const size_t slash = out.find_last_of("\\/");
	if (slash == std::string::npos) return "";
	return out.substr(0, slash + 1);
}

/// Layer 2. Reads the file cameraman.ini. One line gives one value. The
/// character # starts a comment. The function does not fail if the file is not
/// there, except if a person selected the file.
static void read_file(Store& store, const std::string& path, bool must_exist) {
	std::ifstream file(path.c_str());
	if (!file.is_open()) {
		if (must_exist)
			fail("The application cannot open this settings file: " + path);
		return;
	}

	std::string line;
	int number = 0;
	while (std::getline(file, line)) {
		number++;
		const size_t comment = line.find('#');
		if (comment != std::string::npos) line = line.substr(0, comment);
		line = trim(line);
		if (line.empty()) continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			char text[64];
			sprintf_s(text, sizeof(text), "%d", number);
			fail("Line " + std::string(text) + " of " + path +
				" has no = character:\n\n" + line +
				"\n\nThe format is <name> = <value>.");
		}
		const std::string key = trim(line.substr(0, eq));
		const std::string value = trim(line.substr(eq + 1));
		if (!is_known_key(key)) {
			fail("This name in " + path + " is not known: " + key +
				"\n\nThe names are:\n" + all_key_names());
		}
		Entry e = { value, path };
		store[key] = e;
	}
}

// ----------------------------------------------------------------
// The values

static std::string get_str(const Store& s, const char* key, const std::string& def) {
	const Store::const_iterator it = s.find(key);
	if (it == s.end()) return def;
	return it->second.value;
}

static uint32_t get_u32(const Store& s, const char* key, uint32_t def) {
	const Store::const_iterator it = s.find(key);
	if (it == s.end()) return def;
	const std::string& raw = it->second.value;
	if (raw.empty() || raw[0] == '-') {
		fail(std::string("The value of ") + key + " is not a positive number: " + raw);
	}
	char* end = NULL;
	const unsigned long long value = _strtoui64(raw.c_str(), &end, 0);
	if (end == raw.c_str() || *end != '\0') {
		fail(std::string("The value of ") + key + " is not a number: " + raw);
	}
	if (value > 0xFFFFFFFFull) {
		fail(std::string("The value of ") + key + " is too large: " + raw);
	}
	return (uint32_t)value;
}

static double get_dbl(const Store& s, const char* key, double def) {
	const Store::const_iterator it = s.find(key);
	if (it == s.end()) return def;
	const std::string& raw = it->second.value;
	char* end = NULL;
	const double value = strtod(raw.c_str(), &end);
	if (end == raw.c_str() || *end != '\0') {
		fail(std::string("The value of ") + key + " is not a number: " + raw);
	}
	return value;
}

static bool get_bool(const Store& s, const char* key, bool def) {
	const Store::const_iterator it = s.find(key);
	if (it == s.end()) return def;
	const std::string value = to_upper(it->second.value);
	if (value == "1" || value == "TRUE" || value == "YES" || value == "ON")
		return true;
	if (value == "0" || value == "FALSE" || value == "NO" || value == "OFF")
		return false;
	fail(std::string("The value of ") + key + " is not true or false: " +
		it->second.value + "\n\nUse 1, 0, true, false, yes, no, on, or off.");
	return def;
}

// ----------------------------------------------------------------

Config load_config() {
	Store store;

	// Read the command line first, to find the name of the settings file.
	Store command_line;
	read_command_line(command_line);

	std::string file_path;
	bool must_exist = false;
	const Store::const_iterator from_cmd = command_line.find("config");
	if (from_cmd != command_line.end()) {
		file_path = from_cmd->second.value;
		must_exist = true;
	}
	else {
		std::string from_env;
		if (env_get("PLSCM_CONFIG", from_env)) {
			file_path = trim(from_env);
			must_exist = true;
		}
		else {
			file_path = executable_directory() + "cameraman.ini";
		}
	}

	// Layer 2, then 3, then 4. A later layer replaces an earlier layer.
	read_file(store, file_path, must_exist);
	read_environment(store);
	for (Store::const_iterator it = command_line.begin();
		it != command_line.end(); ++it) {
		store[it->first] = it->second;
	}

	Config c;
	c.server_host = get_str(store, "server_host", defaults::SERVER_HOST);
	const uint32_t port = get_u32(store, "server_port", defaults::SERVER_PORT);
	if (port > 65535) {
		fail("server_port must be from 1 to 65535. The value is: " +
			std::to_string(port));
	}
	c.server_port = (uint16_t)port;
	c.send_enable = get_bool(store, "send_enable", defaults::SEND_ENABLE);
	c.zstd_level = (int)get_u32(store, "zstd_level", (uint32_t)defaults::ZSTD_LEVEL);

	c.camera_count = get_u32(store, "camera_count", defaults::CAMERA_COUNT);
	c.frame_width = get_u32(store, "frame_width", defaults::FRAME_WIDTH);
	c.frame_height = get_u32(store, "frame_height", defaults::FRAME_HEIGHT);
	c.frame_bytes_per_px = get_u32(store, "frame_bytes_per_px", defaults::FRAME_BYTES_PER_PX);
	c.io_threads = get_u32(store, "io_threads", defaults::IO_THREADS);
	c.dcam_buffers = get_u32(store, "dcam_buffers", defaults::DCAM_BUFFERS);
	c.max_queue_depth = get_u32(store, "max_queue_depth", defaults::MAX_QUEUE_DEPTH);

	c.h_interval = get_dbl(store, "h_interval", defaults::H_INTERVAL);
	c.exposure_time = get_dbl(store, "exposure_time", defaults::EXPOSURE_TIME);
	c.trigger_interval = get_dbl(store, "trigger_interval", defaults::TRIGGER_INTERVAL);
	c.hsync = get_u32(store, "hsync", defaults::HSYNC);

	c.preview_scale = get_u32(store, "preview_scale", defaults::PREVIEW_SCALE);

	c.demo_mode = get_bool(store, "demo_mode", defaults::DEMO_MODE);
	c.demo_interval_ms = get_u32(store, "demo_interval_ms", defaults::DEMO_INTERVAL_MS);
	c.demo_pattern = get_str(store, "demo_pattern", defaults::DEMO_PATTERN);
	c.demo_file = get_str(store, "demo_file", defaults::DEMO_FILE);
	c.demo_noise_bits = get_u32(store, "demo_noise_bits", defaults::DEMO_NOISE_BITS);
	c.demo_expid = get_u32(store, "demo_expid", defaults::DEMO_EXPID);

	// ---- Tests of the values ----

	if (c.camera_count < 1 || c.camera_count > MAX_CAMERAS) {
		fail("camera_count must be from 1 to " + std::to_string(MAX_CAMERAS) +
			".\n\nMAX_CAMERAS in config.h gives the size of the arrays of the"
			" application. To use more cameras, increase MAX_CAMERAS and build"
			" the application again.");
	}
	if (c.frame_width < 1 || c.frame_height < 1) {
		fail("frame_width and frame_height must be larger than 0.");
	}
	if (c.frame_bytes_per_px != 2) {
		fail("frame_bytes_per_px must be 2.\n\nThe preview code and the header"
			" of the frame protocol use 16-bit pixels.");
	}
	if (c.preview_scale < 1) {
		fail("preview_scale must be larger than 0.");
	}
	if (c.frame_width % c.preview_scale != 0 ||
		c.frame_height % c.preview_scale != 0) {
		fail("frame_width and frame_height must be a multiple of"
			" preview_scale.\n\nIf they are not, the preview does not show the"
			" full frame.");
	}
	if (c.io_threads < 1) {
		fail("io_threads must be larger than 0.");
	}
	if (c.dcam_buffers < 2) {
		fail("dcam_buffers must be 2 or more.");
	}
	if (c.zstd_level < 1 || c.zstd_level > 22) {
		fail("zstd_level must be from 1 to 22. Level 1 is the fastest.");
	}
	if (c.send_enable && c.server_port == 0) {
		fail("server_port must be from 1 to 65535.");
	}
	if (c.send_enable && c.server_host.empty()) {
		fail("server_host is empty.\n\nGive an address, or set send_enable = 0"
			" to test the application without a server.");
	}
	if (c.demo_noise_bits < 1 || c.demo_noise_bits > 16) {
		fail("demo_noise_bits must be from 1 to 16.");
	}
	if (c.demo_pattern != "bars" && c.demo_pattern != "noise" &&
		c.demo_pattern != "file") {
		fail("demo_pattern must be bars, noise, or file. The value is: " +
			c.demo_pattern);
	}
	if (c.demo_mode && c.demo_pattern == "file" && c.demo_file.empty()) {
		fail("demo_pattern is file, but demo_file is empty.\n\nGive the path of"
			" a frame file. The file is one member of a zip archive of the"
			" storage host, or a raw frame.");
	}

	// ---- The derived values ----

	c.frame_bytes = (size_t)c.frame_width * c.frame_height * c.frame_bytes_per_px;
	c.panel_width = c.frame_width / c.preview_scale;
	c.panel_height = c.frame_height / c.preview_scale;

	// A Windows bitmap of 24 bits needs a row of a multiple of 4 bytes. If the
	// row is not a multiple of 4, the preview is not straight.
	const size_t row_bytes = (size_t)c.panel_width * c.camera_count * 3;
	c.preview_stride = ((row_bytes + 3) / 4) * 4;
	c.preview_bytes = c.preview_stride * c.panel_height;

	g_store = store;
	return c;
}

// ----------------------------------------------------------------

/// Writes one line with the name, the value, and the origin.
static void show_line(const char* key, const std::string& value) {
	const Store::const_iterator it = g_store.find(key);
	const char* origin = (it == g_store.end()) ? "default" : it->second.origin.c_str();
	printf("  %-20s = %-24s (%s)\n", key, value.c_str(), origin);
}

static void show_line(const char* key, uint64_t value) {
	show_line(key, std::to_string(value));
}

static void show_line(const char* key, double value) {
	char text[64];
	sprintf_s(text, sizeof(text), "%.9g", value);
	show_line(key, std::string(text));
}

static void show_line(const char* key, bool value) {
	show_line(key, std::string(value ? "1" : "0"));
}

void show_config(const Config& c) {
	printf("Configuration:\n");
	show_line("server_host", c.server_host);
	show_line("server_port", (uint64_t)c.server_port);
	show_line("send_enable", c.send_enable);
	show_line("zstd_level", (uint64_t)c.zstd_level);
	show_line("camera_count", (uint64_t)c.camera_count);
	show_line("frame_width", (uint64_t)c.frame_width);
	show_line("frame_height", (uint64_t)c.frame_height);
	show_line("frame_bytes_per_px", (uint64_t)c.frame_bytes_per_px);
	show_line("io_threads", (uint64_t)c.io_threads);
	show_line("dcam_buffers", (uint64_t)c.dcam_buffers);
	show_line("max_queue_depth", (uint64_t)c.max_queue_depth);
	show_line("h_interval", c.h_interval);
	show_line("exposure_time", c.exposure_time);
	show_line("trigger_interval", c.trigger_interval);
	show_line("hsync", (uint64_t)c.hsync);
	show_line("preview_scale", (uint64_t)c.preview_scale);
	show_line("demo_mode", c.demo_mode);
	show_line("demo_interval_ms", (uint64_t)c.demo_interval_ms);
	show_line("demo_pattern", c.demo_pattern);
	show_line("demo_file", c.demo_file);
	show_line("demo_noise_bits", (uint64_t)c.demo_noise_bits);
	show_line("demo_expid", (uint64_t)c.demo_expid);

	const double dcam_gb =
		(double)c.dcam_buffers * c.frame_bytes * c.camera_count / 1073741824.0;
	printf("Calculated values:\n");
	printf("  one frame            = %zu bytes\n", c.frame_bytes);
	printf("  preview panel        = %u x %u pixels\n", c.panel_width, c.panel_height);
	printf("  preview area         = %zu bytes\n", c.preview_bytes);
	if (!c.demo_mode)
		printf("  camera buffers       = %.1f GB of RAM for %u cameras\n",
			dcam_gb, c.camera_count);
	printf("\n");

	// The default address is an example. It is not a real host.
	if (c.send_enable && c.server_host == defaults::SERVER_HOST) {
		printf("WARNING: server_host is the example address %s.\n",
			defaults::SERVER_HOST);
		printf("         The application cannot send the frames. Give the\n");
		printf("         address of the storage host in cameraman.ini, in\n");
		printf("         PLSCM_SERVER_HOST, or with --server_host= .\n\n");
	}
	fflush(stdout);
}
