/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "command_session.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

#include "cli_runtime.hpp"
#include "core/common.hpp"
#include "settings_store.hpp"
#include "storage.hpp"

namespace fanctl {

namespace {

constexpr size_t kMaxArgs = 24;

struct SessionEmitContext {
	SessionWriteFn writer;
	void *ctx;
};

void SessionEmitLine(void *ctx, const char *line)
{
	SessionEmitContext *emit = static_cast<SessionEmitContext *>(ctx);
	if (emit == nullptr || emit->writer == nullptr || line == nullptr) {
		return;
	}

	emit->writer(emit->ctx, line, strlen(line));
	emit->writer(emit->ctx, "\r\n", 2U);
}

void SessionWrite(void *ctx, const char *text, size_t len)
{
	SessionEmitContext *emit = static_cast<SessionEmitContext *>(ctx);
	if (emit == nullptr || emit->writer == nullptr || text == nullptr) {
		return;
	}

	emit->writer(emit->ctx, text, len);
}

cli::State MakeState(LineEditor &editor, char *cwd, size_t cwd_len)
{
	cli::State state = {};
	state.editor = &editor;
	state.cwd = cwd;
	state.cwd_len = cwd_len;
	return state;
}

cli::Io MakeIo(SessionEmitContext *emit)
{
	cli::Io io = {};
	io.write = SessionWrite;
	io.line = SessionEmitLine;
	io.ctx = emit;
	return io;
}

} // namespace

CommandSession::CommandSession(const ServiceContext &services)
	: services_(services)
{
	Reset();
}

void CommandSession::Reset()
{
	cli::State state = MakeState(editor_, cwd_, sizeof(cwd_));
	cli::ResetState(&state);
}

void CommandSession::BuildPrompt(char *buffer, size_t buffer_len) const
{
	cli::State state = MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				     sizeof(cwd_));
	cli::BuildPrompt(state, buffer, buffer_len);
}

void CommandSession::Emit(SessionWriteFn writer, void *ctx, const char *text) const
{
	if (writer == nullptr || text == nullptr) {
		return;
	}

	writer(ctx, text, strlen(text));
}

void CommandSession::Emitf(SessionWriteFn writer, void *ctx, const char *fmt, ...) const
{
	if (writer == nullptr || fmt == nullptr) {
		return;
	}

	char buffer[512];
	va_list args;

	va_start(args, fmt);
	int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (written <= 0) {
		return;
	}

	writer(ctx, buffer, static_cast<size_t>(MIN(written, static_cast<int>(sizeof(buffer) - 1))));
}

int CommandSession::ResolvePath(const char *input, char *output, size_t output_len) const
{
	cli::State state = MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				     sizeof(cwd_));
	return cli::ResolvePath(state, input, output, output_len);
}

int CommandSession::Tokenize(char *line, char *argv[], size_t argv_len) const
{
	if (line == nullptr || argv == nullptr || argv_len == 0U) {
		return -EINVAL;
	}

	size_t argc = 0U;
	char *cursor = line;

	while (*cursor != '\0') {
		while (*cursor != '\0' && isspace(static_cast<unsigned char>(*cursor)) != 0) {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}

		if (argc >= argv_len) {
			return -ENOSPC;
		}

		char quote = '\0';
		if (*cursor == '"' || *cursor == '\'') {
			quote = *cursor++;
		}

		argv[argc++] = cursor;
		while (*cursor != '\0') {
			if (quote != '\0') {
				if (*cursor == quote) {
					*cursor++ = '\0';
					break;
				}
				++cursor;
				continue;
			}

			if (isspace(static_cast<unsigned char>(*cursor)) != 0) {
				*cursor++ = '\0';
				break;
			}
			++cursor;
		}
	}

	return static_cast<int>(argc);
}

int CommandSession::JoinTokens(char *buffer, size_t buffer_len, char *argv[], int argc, int start) const
{
	return cli::JoinTokens(buffer, buffer_len, argv, argc, start);
}

int CommandSession::ApplyConfigFromStore() const
{
	return cli::ApplyConfigFromStore(services_);
}

int CommandSession::HandleLs(const char *path, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleLs(services_,
			     MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				      sizeof(cwd_)),
			     path, MakeIo(&emit));
}

int CommandSession::HandleCat(const char *path, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleCat(services_,
			      MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				       sizeof(cwd_)),
			      path, MakeIo(&emit));
}

int CommandSession::HandleMkdir(const char *path, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleMkdir(services_,
				MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
					 sizeof(cwd_)),
				path, MakeIo(&emit));
}

int CommandSession::HandleRm(const char *path, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleRm(services_,
			     MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				      sizeof(cwd_)),
			     path, MakeIo(&emit));
}

int CommandSession::HandleTouch(const char *path, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleTouch(services_,
				MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
					 sizeof(cwd_)),
				path, MakeIo(&emit));
}

int CommandSession::HandleWriteFile(char *argv[], int argc, SessionWriteFn writer, void *ctx)
{
	SessionEmitContext emit = { writer, ctx };
	return cli::HandleWriteFile(services_, MakeState(editor_, cwd_, sizeof(cwd_)), argv, argc,
				    MakeIo(&emit));
}

int CommandSession::HandleFanctl(char *argv[], int argc, SessionWriteFn writer, void *ctx,
				 CommandSessionResult *result)
{
	SessionEmitContext emit = { writer, ctx };
	cli::CommandResult cli_result = {};
	cli::State state = MakeState(editor_, cwd_, sizeof(cwd_));
	int rc = cli::HandleFanctl(services_, state, argv, argc, MakeIo(&emit), &cli_result);
	if (result != nullptr) {
		result->exit_requested = cli_result.exit_requested;
		result->reboot_requested = cli_result.reboot_requested;
	}
	return rc;
}

int CommandSession::HandleShow(char *argv[], int argc, SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	cli::State state = MakeState(const_cast<LineEditor &>(editor_), const_cast<char *>(cwd_),
				     sizeof(cwd_));
	return cli::HandleShow(services_, state, argv, argc, MakeIo(&emit));
}

int CommandSession::HandleEdit(char *argv[], int argc, SessionWriteFn writer, void *ctx)
{
	SessionEmitContext emit = { writer, ctx };
	cli::State state = MakeState(editor_, cwd_, sizeof(cwd_));
	return cli::HandleEdit(services_, state, argv, argc, MakeIo(&emit));
}

void CommandSession::EmitHelp(SessionWriteFn writer, void *ctx) const
{
	SessionEmitContext emit = { writer, ctx };
	cli::EmitHelp(MakeIo(&emit));
}

int CommandSession::Execute(const char *command_line, SessionWriteFn writer, void *ctx,
			    CommandSessionResult *result)
{
	if (result != nullptr) {
		result->exit_requested = false;
		result->reboot_requested = false;
	}

	if (command_line == nullptr) {
		return -EINVAL;
	}

	char line[512];
	(void)snprintf(line, sizeof(line), "%s", command_line);

	size_t line_len = strlen(line);
	while (line_len > 0U && (line[line_len - 1U] == '\r' || line[line_len - 1U] == '\n')) {
		line[--line_len] = '\0';
	}

	char *argv[kMaxArgs];
	int argc = Tokenize(line, argv, ARRAY_SIZE(argv));
	if (argc < 0) {
		Emit(writer, ctx, "parse error\r\n");
		return argc;
	}
	if (argc == 0) {
		return 0;
	}

	if (strcmp(argv[0], "help") == 0) {
		EmitHelp(writer, ctx);
		return 0;
	}

	if (strcmp(argv[0], "pwd") == 0) {
		SessionEmitContext emit = { writer, ctx };
		return cli::HandlePwd(MakeState(editor_, cwd_, sizeof(cwd_)), MakeIo(&emit));
	}

	if (strcmp(argv[0], "cd") == 0) {
		SessionEmitContext emit = { writer, ctx };
		cli::State state = MakeState(editor_, cwd_, sizeof(cwd_));
		return cli::HandleCd(services_, &state, argc > 1 ? argv[1] : "/root", MakeIo(&emit));
	}

	if (strcmp(argv[0], "ls") == 0) {
		return HandleLs(argc > 1 ? argv[1] : cwd_, writer, ctx);
	}

	if (strcmp(argv[0], "cat") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: cat <path>\r\n");
			return -EINVAL;
		}
		return HandleCat(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "mkdir") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: mkdir <path>\r\n");
			return -EINVAL;
		}
		return HandleMkdir(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "touch") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: touch <path>\r\n");
			return -EINVAL;
		}
		return HandleTouch(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "rm") == 0) {
		if (argc < 2) {
			Emit(writer, ctx, "usage: rm <path>\r\n");
			return -EINVAL;
		}
		return HandleRm(argv[1], writer, ctx);
	}

	if (strcmp(argv[0], "writefile") == 0) {
		if (argc < 3) {
			Emit(writer, ctx, "usage: writefile <path> <text>\r\n");
			return -EINVAL;
		}
		return HandleWriteFile(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "cf") == 0 || strcmp(argv[0], "duf") == 0) {
		SessionEmitContext emit = { writer, ctx };
		return cli::HandleStorageSummary(MakeIo(&emit));
	}

	if (strcmp(argv[0], "fanctl") == 0) {
		return HandleFanctl(argv, argc, writer, ctx, result);
	}

	if (strcmp(argv[0], "show") == 0) {
		return HandleShow(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "edit") == 0) {
		return HandleEdit(argv, argc, writer, ctx);
	}

	if (strcmp(argv[0], "top") == 0 || strcmp(argv[0], "htop") == 0) {
		SessionEmitContext emit = { writer, ctx };
		return cli::HandleTop(services_, argv, argc, MakeIo(&emit));
	}

	if (strcmp(argv[0], "whoami") == 0) {
		Emit(writer, ctx, "root\r\n");
		return 0;
	}

	if (strcmp(argv[0], "hostname") == 0) {
		Emitf(writer, ctx, "%s\r\n", kDeviceHostname);
		return 0;
	}

	if (strcmp(argv[0], "uname") == 0) {
		Emit(writer, ctx, "Zephyr fanctl\r\n");
		return 0;
	}

	if (strcmp(argv[0], "echo") == 0) {
		if (argc > 1) {
			char buffer[384];
			int rc = JoinTokens(buffer, sizeof(buffer), argv, argc, 1);
			if (rc != 0) {
				Emit(writer, ctx, "echo: text too long\r\n");
				return rc;
			}
			Emitf(writer, ctx, "%s\r\n", buffer);
		} else {
			Emit(writer, ctx, "\r\n");
		}
		return 0;
	}

	if (strcmp(argv[0], "clear") == 0) {
		Emit(writer, ctx, "\033[2J\033[H");
		return 0;
	}

	if (strcmp(argv[0], "exit") == 0) {
		if (result != nullptr) {
			result->exit_requested = true;
		}
		return 0;
	}

	if (strcmp(argv[0], "reboot") == 0) {
		Emit(writer, ctx, "rebooting...\r\n");
		if (result != nullptr) {
			result->reboot_requested = true;
			result->exit_requested = true;
		}
		return 0;
	}

	Emitf(writer, ctx, "%s: command not found\r\n", argv[0]);
	return -ENOENT;
}

namespace {

const char *kTopLevelCommands[] = {
	"help", "pwd", "cd", "ls", "cat", "cf", "duf", "touch", "mkdir", "rm",
	"writefile", "fanctl", "show", "edit", "whoami", "hostname",
	"uname", "echo", "clear", "top", "htop", "exit", "reboot", nullptr
};

const char *kFanctlSubcommands[] = {
	"status", "set", "wifi", "ap", "config", "clearwifi", "factoryreset", nullptr
};

const char *kShowSubcommands[] = {
	"fans", "curves", "system", "wifi", "storage", "host", "ntp", "ssh", "all", nullptr
};

const char *kEditSubcommands[] = {
	"open", "status", "show", "set", "ins", "app", "del", "write", "saveas", "quit", nullptr
};

const char *kApOptions[] = { "on", "off", nullptr };

} // namespace

int CommandSession::Complete(const char *command_line, SessionWriteFn writer, void *ctx,
			     char *completion, size_t completion_len)
{
	if (command_line == nullptr || completion == nullptr || completion_len == 0U) {
		return -EINVAL;
	}

	completion[0] = '\0';

	// Parse command line to get current word
	char line[256];
	snprintf(line, sizeof(line), "%s", command_line);

	// Find the last word being typed
	char *last_word = line;
	char *cursor = line;
	bool in_space = false;
	int argc = 0;

	while (*cursor != '\0') {
		if (*cursor == ' ') {
			if (!in_space) {
				in_space = true;
				*cursor = '\0';
				last_word = cursor + 1;
				argc++;
			}
		} else {
			in_space = false;
		}
		cursor++;
	}

	// Find the command being typed (first argument)
	char *first_arg = line;
	char *second_arg = nullptr;
	bool found_space = false;

	for (char *p = line; *p != '\0'; p++) {
		if (*p == ' ') {
			*p = '\0';
			if (!found_space) {
				second_arg = p + 1;
				found_space = true;
			}
		}
	}

	const char *prefix = last_word;
	size_t prefix_len = strlen(prefix);
	const char **candidates = nullptr;
	const char *match = nullptr;
	int match_count = 0;

	// Determine which set of candidates to use based on context
	if (!found_space || (found_space && second_arg == last_word)) {
		// Completing top-level command
		if (strcmp(first_arg, "fanctl") == 0) {
			candidates = kFanctlSubcommands;
		} else if (strcmp(first_arg, "show") == 0) {
			candidates = kShowSubcommands;
		} else if (strcmp(first_arg, "edit") == 0) {
			candidates = kEditSubcommands;
		} else if (strcmp(first_arg, "ap") == 0) {
			candidates = kApOptions;
		} else {
			candidates = kTopLevelCommands;
		}
	} else {
		// Completing arguments for subcommands
		if (strcmp(first_arg, "fanctl") == 0) {
			// fanctl set <fan> <percent> <on|off>
			if (second_arg != nullptr && strcmp(second_arg, "set") == 0) {
				// Check which argument position we're at
				int set_argc = 0;
				for (char *p = line; *p != '\0'; p++) {
					if (*p == ' ') set_argc++;
				}
				// Count spaces after "set"
				int spaces_after_set = 0;
				bool in_set = false;
				for (const char *p = command_line; *p != '\0'; p++) {
					if (!in_set && strncmp(p, "set ", 4) == 0) {
						in_set = true;
						p += 3;
						continue;
					}
					if (in_set && *p == ' ') spaces_after_set++;
				}
				if (spaces_after_set == 2) {
					// Completing on/off
					candidates = kApOptions;
				}
			}
		} else if (strcmp(first_arg, "ap") == 0 ||
			   (second_arg != nullptr && strcmp(second_arg, "ap") == 0)) {
			candidates = kApOptions;
		}
	}

	// Find matches
	if (candidates != nullptr) {
		for (int i = 0; candidates[i] != nullptr; i++) {
			if (strncmp(candidates[i], prefix, prefix_len) == 0) {
				match_count++;
				if (match == nullptr) {
					match = candidates[i];
				}
			}
		}
	}

	// Handle completion result
	if (match_count == 1 && match != nullptr) {
		// Single match - complete it
		const char *suffix = match + prefix_len;
		if (suffix[0] != '\0') {
			snprintf(completion, completion_len, "%s", suffix);
			// Add space if completing a full command/subcommand
			if (completion_len > strlen(completion) + 1) {
				strcat(completion, " ");
			}
		}
		return 1;
	} else if (match_count > 1) {
		// Multiple matches - show options
		Emit(writer, ctx, "\r\n");
		for (int i = 0; candidates[i] != nullptr; i++) {
			if (strncmp(candidates[i], prefix, prefix_len) == 0) {
				Emitf(writer, ctx, "  %s", candidates[i]);
			}
		}
		Emit(writer, ctx, "\r\n");
		// Redraw prompt
		char prompt[64];
		BuildPrompt(prompt, sizeof(prompt));
		Emit(writer, ctx, prompt);
		Emit(writer, ctx, command_line);
		return match_count;
	}

	return 0;
}

} // namespace fanctl
