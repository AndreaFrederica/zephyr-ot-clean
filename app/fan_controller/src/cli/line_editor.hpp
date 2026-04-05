/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_LINE_EDITOR_HPP_
#define FAN_CONTROLLER_LINE_EDITOR_HPP_

#include <stddef.h>

namespace fanctl {

class LineEditor {
public:
	LineEditor();

	int Open(const char *path);
	void Close();
	bool IsOpen() const;
	bool IsDirty() const;
	const char *GetPath() const;
	const char *GetContent() const;
	size_t GetLineCount() const;
	int PrintLines(char *out, size_t out_len, size_t start_line, size_t end_line) const;
	int ReplaceLine(size_t line_no, const char *text);
	int InsertLine(size_t line_no, const char *text);
	int AppendLine(const char *text);
	int DeleteLine(size_t line_no);

private:
	int RewriteWithSingleLineChange(size_t line_no, const char *text, bool replace, bool remove);
	int CopyLineRange(char *dst, size_t dst_len, size_t line_index, size_t *out_len) const;

	static constexpr size_t kMaxPathLen = 128;
	static constexpr size_t kMaxBufferLen = 2048;

	char path_[kMaxPathLen];
	char buffer_[kMaxBufferLen];
	bool open_;
	bool dirty_;
};

} // namespace fanctl

#endif
