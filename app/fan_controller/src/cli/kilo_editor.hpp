/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded adaptation of antirez/kilo text editor for use over SSH/serial
 * on Zephyr RTOS. Original kilo is BSD-2-Clause licensed.
 */

#ifndef FAN_CONTROLLER_KILO_EDITOR_HPP_
#define FAN_CONTROLLER_KILO_EDITOR_HPP_

#include <stddef.h>

#include "core/memory_domains.hpp"

namespace fanctl::kilo {

using ReadCharFn = int (*)(void *ctx);
using PeekCharFn = int (*)(void *ctx, char *ch);
using WriteFn = void (*)(void *ctx, const char *data, size_t len);

struct Io {
	ReadCharFn read_char;
	PeekCharFn peek_char;
	WriteFn write;
	void *ctx;
};

int Run(const Io &io, const char *path, int screen_rows, int screen_cols);
bool GetHeapSnapshot(memory::HeapSnapshot *snapshot);

} // namespace fanctl::kilo

#endif
