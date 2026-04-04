/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "line_editor.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "storage.hpp"

namespace fanctl {

LineEditor::LineEditor() : open_(false), dirty_(false)
{
	path_[0] = '\0';
	buffer_[0] = '\0';
}

int LineEditor::Open(const char *path)
{
	if (path == nullptr || path[0] == '\0') {
		return -EINVAL;
	}

	char resolved[kMaxPathLen];
	int rc = storage::ResolveManagedPath(path, resolved, sizeof(resolved));
	if (rc != 0) {
		return rc;
	}

	size_t out_len = 0U;
	rc = storage::ReadTextFile(path, buffer_, sizeof(buffer_), &out_len);
	if (rc != 0 && rc != -ENOENT) {
		return rc;
	}

	if (rc == -ENOENT) {
		buffer_[0] = '\0';
	}

	(void)snprintf(path_, sizeof(path_), "%s", path);
	open_ = true;
	dirty_ = false;
	return 0;
}

void LineEditor::Close()
{
	open_ = false;
	dirty_ = false;
	path_[0] = '\0';
	buffer_[0] = '\0';
}

bool LineEditor::IsOpen() const
{
	return open_;
}

bool LineEditor::IsDirty() const
{
	return dirty_;
}

const char *LineEditor::GetPath() const
{
	return path_;
}

const char *LineEditor::GetContent() const
{
	return buffer_;
}

size_t LineEditor::GetLineCount() const
{
	if (!open_ || buffer_[0] == '\0') {
		return 0;
	}

	size_t lines = 1U;
	for (size_t i = 0U; buffer_[i] != '\0'; ++i) {
		if (buffer_[i] == '\n') {
			++lines;
		}
	}

	return lines;
}

int LineEditor::CopyLineRange(char *dst, size_t dst_len, size_t line_index, size_t *out_len) const
{
	if (!open_ || dst == nullptr || dst_len == 0U) {
		return -EINVAL;
	}

	size_t current_line = 1U;
	size_t src_pos = 0U;
	size_t dst_pos = 0U;

	while (buffer_[src_pos] != '\0' && current_line < line_index) {
		if (buffer_[src_pos++] == '\n') {
			++current_line;
		}
	}

	if (current_line != line_index) {
		return -ENOENT;
	}

	while (buffer_[src_pos] != '\0' && buffer_[src_pos] != '\n' && dst_pos + 1U < dst_len) {
		dst[dst_pos++] = buffer_[src_pos++];
	}

	if (dst_pos >= dst_len) {
		return -ENOSPC;
	}

	dst[dst_pos] = '\0';
	if (out_len != nullptr) {
		*out_len = dst_pos;
	}

	return 0;
}

int LineEditor::PrintLines(char *out, size_t out_len, size_t start_line, size_t end_line) const
{
	if (!open_ || out == nullptr || out_len == 0U) {
		return -EINVAL;
	}

	if (start_line == 0U) {
		start_line = 1U;
	}

	size_t total_lines = GetLineCount();
	if (total_lines == 0U) {
		return snprintf(out, out_len, "(empty)\n") < static_cast<int>(out_len) ? 0 : -ENOSPC;
	}

	if (end_line == 0U || end_line > total_lines) {
		end_line = total_lines;
	}

	if (start_line > end_line || start_line > total_lines) {
		return -EINVAL;
	}

	size_t offset = 0U;
	char line[192];

	for (size_t i = start_line; i <= end_line; ++i) {
		int rc = CopyLineRange(line, sizeof(line), i, nullptr);
		if (rc != 0) {
			return rc;
		}

		int written = snprintf(out + offset, out_len - offset, "%u: %s\n",
				       static_cast<unsigned int>(i), line);
		if (written <= 0 || static_cast<size_t>(written) >= out_len - offset) {
			return -ENOSPC;
		}

		offset += static_cast<size_t>(written);
	}

	return 0;
}

int LineEditor::RewriteWithSingleLineChange(size_t line_no, const char *text, bool replace, bool remove)
{
	if (!open_ || line_no == 0U || text == nullptr) {
		return -EINVAL;
	}

	char updated[kMaxBufferLen];
	size_t src = 0U;
	size_t dst = 0U;
	size_t current_line = 1U;
	bool touched = false;

	while (buffer_[src] != '\0') {
		size_t line_start = src;
		while (buffer_[src] != '\0' && buffer_[src] != '\n') {
			++src;
		}
		size_t line_len = src - line_start;
		bool has_newline = (buffer_[src] == '\n');

		if (current_line == line_no) {
			touched = true;

			if (!remove) {
				int written = snprintf(updated + dst, kMaxBufferLen - dst, "%s", text);
				if (written < 0 || static_cast<size_t>(written) >= kMaxBufferLen - dst) {
					return -ENOSPC;
				}
				dst += static_cast<size_t>(written);
				if (replace && (has_newline || buffer_[src] != '\0')) {
					if (dst + 1U >= kMaxBufferLen) {
						return -ENOSPC;
					}
					updated[dst++] = '\n';
				}
			}

			if (!replace && !remove) {
				if (dst + line_len + 1U >= kMaxBufferLen) {
					return -ENOSPC;
				}
				memcpy(updated + dst, buffer_ + line_start, line_len);
				dst += line_len;
				updated[dst++] = '\n';
			}
		} else {
			if (dst + line_len + 2U >= kMaxBufferLen) {
				return -ENOSPC;
			}
			memcpy(updated + dst, buffer_ + line_start, line_len);
			dst += line_len;
			if (has_newline) {
				updated[dst++] = '\n';
			}
		}

		if (has_newline) {
			++src;
		}
		++current_line;
	}

	if (!touched) {
		if (!replace && !remove && line_no == current_line) {
			int written = snprintf(updated + dst, kMaxBufferLen - dst, "%s", text);
			if (written < 0 || static_cast<size_t>(written) >= kMaxBufferLen - dst) {
				return -ENOSPC;
			}
			dst += static_cast<size_t>(written);
			touched = true;
		} else {
			return -ENOENT;
		}
	}

	updated[dst] = '\0';
	memcpy(buffer_, updated, dst + 1U);
	dirty_ = true;
	return 0;
}

int LineEditor::ReplaceLine(size_t line_no, const char *text)
{
	return RewriteWithSingleLineChange(line_no, text, true, false);
}

int LineEditor::InsertLine(size_t line_no, const char *text)
{
	if (!open_ || line_no == 0U || text == nullptr) {
		return -EINVAL;
	}

	if (line_no == 1U && GetLineCount() == 0U) {
		return AppendLine(text);
	}

	if (line_no > GetLineCount()) {
		return -ENOENT;
	}

	char updated[kMaxBufferLen];
	size_t src = 0U;
	size_t dst = 0U;
	size_t current_line = 1U;
	bool inserted = false;

	while (buffer_[src] != '\0') {
		size_t line_start = src;
		while (buffer_[src] != '\0' && buffer_[src] != '\n') {
			++src;
		}
		size_t line_len = src - line_start;
		bool has_newline = (buffer_[src] == '\n');

		if (!inserted && current_line == line_no) {
			int written = snprintf(updated + dst, kMaxBufferLen - dst, "%s\n", text);
			if (written < 0 || static_cast<size_t>(written) >= kMaxBufferLen - dst) {
				return -ENOSPC;
			}
			dst += static_cast<size_t>(written);
			inserted = true;
		}

		if (dst + line_len + 2U >= kMaxBufferLen) {
			return -ENOSPC;
		}
		memcpy(updated + dst, buffer_ + line_start, line_len);
		dst += line_len;
		if (has_newline) {
			updated[dst++] = '\n';
			++src;
		}
		++current_line;
	}

	updated[dst] = '\0';
	memcpy(buffer_, updated, dst + 1U);
	dirty_ = true;
	return 0;
}

int LineEditor::AppendLine(const char *text)
{
	if (!open_ || text == nullptr) {
		return -EINVAL;
	}

	size_t current_len = strlen(buffer_);
	size_t text_len = strlen(text);
	bool needs_newline = (current_len > 0U && buffer_[current_len - 1U] != '\n');
	size_t required = current_len + text_len + (needs_newline ? 1U : 0U) + 1U;

	if (required > sizeof(buffer_)) {
		return -ENOSPC;
	}

	if (needs_newline) {
		buffer_[current_len++] = '\n';
	}

	memcpy(buffer_ + current_len, text, text_len);
	buffer_[current_len + text_len] = '\0';
	dirty_ = true;
	return 0;
}

int LineEditor::DeleteLine(size_t line_no)
{
	return RewriteWithSingleLineChange(line_no, "", true, true);
}

} // namespace fanctl
