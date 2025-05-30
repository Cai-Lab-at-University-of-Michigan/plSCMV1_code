#include <cstdint>
#include <chrono>

typedef uint32_t sendme_int;
typedef uint64_t sendme_timecodet; //ms since epoch

inline sendme_timecodet get_current_timecode() {
	auto now = std::chrono::system_clock::now().time_since_epoch();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

struct sendme {
	uint8_t cameraid;
	sendme_int frameid;
	sendme_int burstid;
	sendme_int expid;
	sendme_timecodet timecode;
	sendme_int payload_size;
};

//                 id, ch, burst, expid, time, buffer_size, pointer
typedef std::tuple<sendme_int, uint8_t, sendme_int, sendme_int, sendme_timecodet, sendme_int, void*> io_tuple;