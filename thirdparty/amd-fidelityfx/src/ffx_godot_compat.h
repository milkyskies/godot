// Godot (hacco #1441): the FidelityFX SDK's host code is written for MSVC; these give gcc and clang what it uses.
#pragma once

#include <cstring>
#include <cwchar>

#ifndef _MSC_VER
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define wcscpy_s(dst, ...) ffx_godot_wcscpy_s(dst, __VA_ARGS__)
template <size_t N>
inline void ffx_godot_wcscpy_s(wchar_t (&p_dst)[N], const wchar_t *p_src) {
	wcsncpy(p_dst, p_src, N - 1);
	p_dst[N - 1] = 0;
}
inline void ffx_godot_wcscpy_s(wchar_t *p_dst, size_t p_size, const wchar_t *p_src) {
	wcsncpy(p_dst, p_src, p_size - 1);
	p_dst[p_size - 1] = 0;
}
#endif
