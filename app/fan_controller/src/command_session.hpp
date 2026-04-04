/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_COMMAND_SESSION_HPP_
#define FAN_CONTROLLER_COMMAND_SESSION_HPP_

#include <stddef.h>

#include "fan_controller.hpp"
#include "host_control_manager.hpp"
#include "line_editor.hpp"
#include "wifi_manager.hpp"

namespace fanctl {

using SessionWriteFn = void (*)(void *ctx, const char *text, size_t len);

struct CommandSessionResult {
	bool exit_requested;
	bool reboot_requested;
};

class CommandSession {
public:
	CommandSession(FanController &fan_controller, WifiManager &wifi_manager,
		       HostControlManager &host_control);

	void Reset();
	void BuildPrompt(char *buffer, size_t buffer_len) const;
	int Execute(const char *command_line, SessionWriteFn writer, void *ctx,
		    CommandSessionResult *result);
	int Complete(const char *command_line, SessionWriteFn writer, void *ctx,
		     char *completion, size_t completion_len);

private:
	void Emit(SessionWriteFn writer, void *ctx, const char *text) const;
	void Emitf(SessionWriteFn writer, void *ctx, const char *fmt, ...) const;
	int ResolvePath(const char *input, char *output, size_t output_len) const;
	int Tokenize(char *line, char *argv[], size_t argv_len) const;
	int JoinTokens(char *buffer, size_t buffer_len, char *argv[], int argc, int start) const;
	int ApplyConfigFromStore() const;
	int HandleFanctl(char *argv[], int argc, SessionWriteFn writer, void *ctx);
	int HandleShow(char *argv[], int argc, SessionWriteFn writer, void *ctx) const;
	int HandleEdit(char *argv[], int argc, SessionWriteFn writer, void *ctx);
	int HandleLs(const char *path, SessionWriteFn writer, void *ctx) const;
	int HandleCat(const char *path, SessionWriteFn writer, void *ctx) const;
	int HandleMkdir(const char *path, SessionWriteFn writer, void *ctx) const;
	int HandleRm(const char *path, SessionWriteFn writer, void *ctx) const;
	int HandleTouch(const char *path, SessionWriteFn writer, void *ctx) const;
	int HandleWriteFile(char *argv[], int argc, SessionWriteFn writer, void *ctx);
	void EmitHelp(SessionWriteFn writer, void *ctx) const;

	FanController &fan_controller_;
	WifiManager &wifi_manager_;
	HostControlManager &host_control_;
	LineEditor editor_;
	char cwd_[128];
};

} // namespace fanctl

#endif
