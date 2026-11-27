/**************************************************************************/
/*  hacco_waits.cpp                                                       */
/**************************************************************************/

#include "hacco_waits.h"

#ifdef HACCO_WAITS

#include "core/profiling/profiling.h"

#include <dlfcn.h>
#include <execinfo.h>

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" char __executable_start;

uint64_t hacco_wait_now() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void hacco_wait_note(const char *p_what, uint64_t p_from_usec, uint64_t p_min_usec, const void *p_function) {
	const uint64_t took = hacco_wait_now() - p_from_usec;
	if (took < p_min_usec) {
		return;
	}
	// The call stack as addresses, each as the binary it is in links it (a PIE's first byte is 0), for addr2line:
	// bare for Godot's own, <file>+0x.. for a library's (the game's).
	char text[2048];
	int n = snprintf(text, sizeof(text), "%s %.1f ms", p_what, took / 1000.0);
	void *frames[24];
	int count = 0;
	if (p_function) {
		frames[0] = const_cast<void *>(p_function);
		count = 1;
	} else {
		count = backtrace(frames, 24);
	}
	// The first is this function (the hooks are inlined into their callers).
	for (int i = p_function ? 0 : 1; i < count && n < (int)sizeof(text) - 80; i++) {
		Dl_info info;
		if (dladdr(frames[i], &info) && info.dli_fbase && info.dli_fbase != (void *)&__executable_start) {
			const char *file = strrchr(info.dli_fname, '/');
			n += snprintf(text + n, sizeof(text) - n, " %s+0x%llx", file ? file + 1 : info.dli_fname, (unsigned long long)((char *)frames[i] - (char *)info.dli_fbase));
		} else {
			n += snprintf(text + n, sizeof(text) - n, " 0x%llx", (unsigned long long)((char *)frames[i] - &__executable_start));
		}
	}
	TracyMessage(text, n);
}

#endif
