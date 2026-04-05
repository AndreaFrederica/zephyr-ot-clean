/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_CLI_RUNTIME_HPP_
#define FAN_CONTROLLER_CLI_RUNTIME_HPP_

#include <stddef.h>

#include "line_editor.hpp"
#include "core/service_context.hpp"

namespace fanctl::cli {

using WriteFn = void (*)(void *ctx, const char *text, size_t len);
using LineFn = void (*)(void *ctx, const char *line);

struct Io {
	WriteFn write;
	LineFn line;
	void *ctx;
};

using Runtime = ServiceContext;

struct State {
	LineEditor *editor;
	char *cwd;
	size_t cwd_len;
};

struct CommandResult {
	bool exit_requested;
	bool reboot_requested;
};

void ResetState(State *state);
void BuildPrompt(const State &state, char *buffer, size_t buffer_len);
void Emit(const Io &io, const char *text);
void EmitLine(const Io &io, const char *text);
void Emitf(const Io &io, const char *fmt, ...);
void EmitLinef(const Io &io, const char *fmt, ...);

int ResolvePath(const State &state, const char *input, char *output, size_t output_len);
int JoinTokens(char *buffer, size_t buffer_len, char *argv[], int argc, int start);
int ApplyConfigFromStore(const Runtime &runtime);

void PrintStatus(const Runtime &runtime, const Io &io);
void EmitHelp(const Io &io);

int HandleLs(const Runtime &runtime, const State &state, const char *path, const Io &io);
int HandleCat(const Runtime &runtime, const State &state, const char *path, const Io &io);
int HandleTouch(const Runtime &runtime, const State &state, const char *path, const Io &io);
int HandleMkdir(const Runtime &runtime, const State &state, const char *path, const Io &io);
int HandleRm(const Runtime &runtime, const State &state, const char *path, const Io &io);
int HandleCp(const Runtime &runtime, const State &state, const char *source, const char *target,
	     const Io &io);
int HandleMv(const Runtime &runtime, const State &state, const char *source, const char *target,
	     const Io &io);
int HandleWriteFile(const Runtime &runtime, const State &state, char *argv[], int argc, const Io &io);
int HandleCd(const Runtime &runtime, State *state, const char *path, const Io &io);
int HandlePwd(const State &state, const Io &io);
int HandleFanctl(const Runtime &runtime, State &state, char *argv[], int argc, const Io &io,
		 CommandResult *result);
int HandleShow(const Runtime &runtime, State &state, char *argv[], int argc, const Io &io);
int HandleTop(const Runtime &runtime, char *argv[], int argc, const Io &io);
int HandleEdit(const Runtime &runtime, State &state, char *argv[], int argc, const Io &io);
int HandleStorageSummary(const Io &io);

} // namespace fanctl::cli

#endif
