/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_HTOP_MONITOR_HPP_
#define FAN_CONTROLLER_HTOP_MONITOR_HPP_

#include <stddef.h>

namespace fanctl::htop {

using ReadCharFn = int (*)(void *ctx);
using PeekCharFn = int (*)(void *ctx, char *ch);
using WriteFn = void (*)(void *ctx, const char *data, size_t len);

struct Io {
	ReadCharFn read_char;
	PeekCharFn peek_char;
	WriteFn write;
	void *ctx;
};

// 运行 htop 监控界面，返回时终端已恢复
// screen_rows/cols: 终端行列数
// return: 0=正常退出, <0=错误
int Run(const Io &io, int screen_rows, int screen_cols);

} // namespace fanctl::htop

#endif // FAN_CONTROLLER_HTOP_MONITOR_HPP_
