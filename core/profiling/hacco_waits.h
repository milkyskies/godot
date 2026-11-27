/**************************************************************************/
/*  hacco_waits.h                                                         */
/**************************************************************************/
/* A debug aid of hacco's (hacco #1261), only in a Tracy build on Linux   */
/* (profiler=tracy): a wait on a contended mutex, the worker pool or the  */
/* render thread, or a render-thread command, past a few milliseconds,    */
/* sent to Tracy as a message with its length and the call stack (as      */
/* addresses for addr2line) that waited.                                  */
/**************************************************************************/

#pragma once

#include "core/profiling/profiling.gen.h"

#include <cstdint>

#if defined(GODOT_USE_TRACY) && defined(__linux__)
#define HACCO_WAITS

// Microseconds on a steady clock.
uint64_t hacco_wait_now();
// Sends "<what> <ms> ms" (and the function's address, when given) when it took p_min_usec or more since p_from_usec.
void hacco_wait_note(const char *p_what, uint64_t p_from_usec, uint64_t p_min_usec, const void *p_function = nullptr);
#endif
