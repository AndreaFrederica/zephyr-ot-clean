/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedded adaptation of antirez/kilo text editor for use over SSH/serial
 * on Zephyr RTOS. Original kilo is BSD-2-Clause licensed.
 */

#include "kilo_editor.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_attr.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>

#include "storage.hpp"

namespace fanctl::kilo {

namespace {

enum KeyAction {
	KEY_NULL = 0,
	CTRL_C = 3,
	CTRL_D = 4,
	CTRL_F = 6,
	CTRL_H = 8,
	TAB = 9,
	CTRL_L = 12,
	ENTER = 13,
	CTRL_Q = 17,
	CTRL_S = 19,
	ESC = 27,
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
};

enum HighlightType {
	HL_NORMAL = 0,
	HL_NONPRINT,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH,
};

constexpr int HL_HIGHLIGHT_STRINGS = 1 << 0;
constexpr int HL_HIGHLIGHT_NUMBERS = 1 << 1;
constexpr size_t kKiloHeapBytes = 4U * 1024U * 1024U;
constexpr size_t kReadChunkBytes = 512U;
constexpr size_t kQueryLen = 128;
constexpr int kQuitTimes = 3;

struct EditorSyntax {
	char **filematch;
	char **keywords;
	char singleline_comment_start[3];
	char multiline_comment_start[3];
	char multiline_comment_end[3];
	int flags;
};

typedef struct EditorRow {
	int idx;
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl;
	int hl_open_comment;
} EditorRow;

struct EditorConfig {
	Io io;
	int cx;
	int cy;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	EditorRow *row;
	int dirty;
	char filename[128];
	char statusmsg[96];
	int64_t statusmsg_time;
	EditorSyntax *syntax;
	bool should_quit;
	int last_error;
};

struct AppendBuffer {
	char *data;
	int len;
};

alignas(sizeof(void *)) EXT_RAM_BSS_ATTR uint8_t g_kilo_heap_storage[kKiloHeapBytes];
struct sys_heap g_kilo_heap;
K_MUTEX_DEFINE(g_kilo_heap_lock);

#define ABUF_INIT                                                                                 \
	{                                                                                           \
		NULL, 0                                                                              \
	}

void ResetKiloHeap(void)
{
	k_mutex_lock(&g_kilo_heap_lock, K_FOREVER);
	sys_heap_init(&g_kilo_heap, g_kilo_heap_storage, sizeof(g_kilo_heap_storage));
	k_mutex_unlock(&g_kilo_heap_lock);
}

void *KiloAlloc(size_t bytes)
{
	if (bytes == 0U) {
		bytes = 1U;
	}
	k_mutex_lock(&g_kilo_heap_lock, K_FOREVER);
	void *ptr = sys_heap_alloc(&g_kilo_heap, bytes);
	k_mutex_unlock(&g_kilo_heap_lock);
	return ptr;
}

void *KiloRealloc(void *ptr, size_t bytes)
{
	if (ptr == NULL) {
		return KiloAlloc(bytes);
	}
	if (bytes == 0U) {
		bytes = 1U;
	}
	k_mutex_lock(&g_kilo_heap_lock, K_FOREVER);
	void *next = sys_heap_realloc(&g_kilo_heap, ptr, bytes);
	k_mutex_unlock(&g_kilo_heap_lock);
	return next;
}

void KiloFree(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	k_mutex_lock(&g_kilo_heap_lock, K_FOREVER);
	sys_heap_free(&g_kilo_heap, ptr);
	k_mutex_unlock(&g_kilo_heap_lock);
}

int EnsureParentDirsForFsPath(const char *path)
{
	char scratch[160];
	if (path == nullptr || strlen(path) >= sizeof(scratch)) {
		return -ENOSPC;
	}

	(void)snprintf(scratch, sizeof(scratch), "%s", path);
	for (size_t i = strlen(storage::GetMountPoint()) + 1U; scratch[i] != '\0'; ++i) {
		if (scratch[i] != '/') {
			continue;
		}

		scratch[i] = '\0';
		int rc = fs_mkdir(scratch);
		scratch[i] = '/';
		if (rc != 0 && rc != -EEXIST) {
			return rc;
		}
	}

	return 0;
}

void ClearEditor(EditorConfig *editor)
{
	for (int i = 0; i < editor->numrows; ++i) {
		KiloFree(editor->row[i].chars);
		KiloFree(editor->row[i].render);
		KiloFree(editor->row[i].hl);
	}
	KiloFree(editor->row);
	editor->row = NULL;
	editor->numrows = 0;
	editor->cx = 0;
	editor->cy = 0;
	editor->rowoff = 0;
	editor->coloff = 0;
	editor->dirty = 0;
}

char *C_HL_extensions[] = { const_cast<char *>(".c"), const_cast<char *>(".h"),
				const_cast<char *>(".cpp"), const_cast<char *>(".hpp"),
				const_cast<char *>(".cc"), NULL };

char *C_HL_keywords[] = {
	const_cast<char *>("auto"),      const_cast<char *>("break"),
	const_cast<char *>("case"),      const_cast<char *>("continue"),
	const_cast<char *>("default"),   const_cast<char *>("do"),
	const_cast<char *>("else"),      const_cast<char *>("enum"),
	const_cast<char *>("extern"),    const_cast<char *>("for"),
	const_cast<char *>("goto"),      const_cast<char *>("if"),
	const_cast<char *>("register"),  const_cast<char *>("return"),
	const_cast<char *>("sizeof"),    const_cast<char *>("static"),
	const_cast<char *>("struct"),    const_cast<char *>("switch"),
	const_cast<char *>("typedef"),   const_cast<char *>("union"),
	const_cast<char *>("volatile"),  const_cast<char *>("while"),
	const_cast<char *>("NULL"),      const_cast<char *>("alignas"),
	const_cast<char *>("alignof"),   const_cast<char *>("and"),
	const_cast<char *>("and_eq"),    const_cast<char *>("asm"),
	const_cast<char *>("bitand"),    const_cast<char *>("bitor"),
	const_cast<char *>("class"),     const_cast<char *>("compl"),
	const_cast<char *>("constexpr"), const_cast<char *>("const_cast"),
	const_cast<char *>("decltype"),  const_cast<char *>("delete"),
	const_cast<char *>("dynamic_cast"), const_cast<char *>("explicit"),
	const_cast<char *>("export"),    const_cast<char *>("false"),
	const_cast<char *>("friend"),    const_cast<char *>("inline"),
	const_cast<char *>("mutable"),   const_cast<char *>("namespace"),
	const_cast<char *>("new"),       const_cast<char *>("noexcept"),
	const_cast<char *>("not"),       const_cast<char *>("not_eq"),
	const_cast<char *>("nullptr"),   const_cast<char *>("operator"),
	const_cast<char *>("or"),        const_cast<char *>("or_eq"),
	const_cast<char *>("private"),   const_cast<char *>("protected"),
	const_cast<char *>("public"),    const_cast<char *>("reinterpret_cast"),
	const_cast<char *>("static_assert"), const_cast<char *>("static_cast"),
	const_cast<char *>("template"),  const_cast<char *>("this"),
	const_cast<char *>("thread_local"), const_cast<char *>("throw"),
	const_cast<char *>("true"),      const_cast<char *>("try"),
	const_cast<char *>("typeid"),    const_cast<char *>("typename"),
	const_cast<char *>("virtual"),   const_cast<char *>("xor"),
	const_cast<char *>("xor_eq"),    const_cast<char *>("int|"),
	const_cast<char *>("long|"),     const_cast<char *>("double|"),
	const_cast<char *>("float|"),    const_cast<char *>("char|"),
	const_cast<char *>("unsigned|"), const_cast<char *>("signed|"),
	const_cast<char *>("void|"),     const_cast<char *>("short|"),
	const_cast<char *>("auto|"),     const_cast<char *>("const|"),
	const_cast<char *>("bool|"),     NULL,
};

EditorSyntax HLDB[] = {
	{ C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
	  HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS },
};

constexpr size_t HLDB_ENTRIES = sizeof(HLDB) / sizeof(HLDB[0]);

void WriteAll(const EditorConfig &editor, const char *data, size_t len)
{
	if (editor.io.write == nullptr || data == nullptr || len == 0U) {
		return;
	}

	editor.io.write(editor.io.ctx, data, len);
}

void Append(AppendBuffer *ab, const char *s, int len)
{
	char *next = static_cast<char *>(KiloRealloc(ab->data, static_cast<size_t>(ab->len + len)));
	if (next == nullptr) {
		return;
	}
	memcpy(next + ab->len, s, static_cast<size_t>(len));
	ab->data = next;
	ab->len += len;
}

void FreeAppendBuffer(AppendBuffer *ab)
{
	KiloFree(ab->data);
	ab->data = NULL;
	ab->len = 0;
}

void SetStatusMessage(EditorConfig *editor, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(editor->statusmsg, sizeof(editor->statusmsg), fmt, args);
	va_end(args);
	editor->statusmsg_time = k_uptime_get();
}

int ReadByte(const EditorConfig &editor)
{
	if (editor.io.read_char == nullptr) {
		return -ENOTSUP;
	}
	return editor.io.read_char(editor.io.ctx);
}

int PeekByte(const EditorConfig &editor, char *ch)
{
	if (editor.io.peek_char == nullptr) {
		return 0;
	}
	return editor.io.peek_char(editor.io.ctx, ch);
}

int ReadKey(const EditorConfig &editor)
{
	int value = ReadByte(editor);
	if (value < 0) {
		return value;
	}

	if (value != ESC) {
		return value;
	}

	char next = '\0';
	if (PeekByte(editor, &next) <= 0) {
		return ESC;
	}

	int first = ReadByte(editor);
	if (first < 0) {
		return ESC;
	}

	if (first == '[') {
		if (PeekByte(editor, &next) <= 0) {
			return ESC;
		}
		int second = ReadByte(editor);
		if (second < 0) {
			return ESC;
		}

		if (second >= '0' && second <= '9') {
			if (PeekByte(editor, &next) <= 0) {
				return ESC;
			}
			int third = ReadByte(editor);
			if (third == '~') {
				switch (second) {
				case '1':
				case '7':
					return HOME_KEY;
				case '3':
					return DEL_KEY;
				case '4':
				case '8':
					return END_KEY;
				case '5':
					return PAGE_UP;
				case '6':
					return PAGE_DOWN;
				default:
					break;
				}
			}
		} else {
			switch (second) {
			case 'A':
				return ARROW_UP;
			case 'B':
				return ARROW_DOWN;
			case 'C':
				return ARROW_RIGHT;
			case 'D':
				return ARROW_LEFT;
			case 'H':
				return HOME_KEY;
			case 'F':
				return END_KEY;
			default:
				break;
			}
		}
	} else if (first == 'O') {
		if (PeekByte(editor, &next) <= 0) {
			return ESC;
		}
		int second = ReadByte(editor);
		switch (second) {
		case 'H':
			return HOME_KEY;
		case 'F':
			return END_KEY;
		default:
			break;
		}
	}

	return ESC;
}

int IsSeparator(int c)
{
	return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL;
}

int RowHasOpenComment(EditorRow *row)
{
	if (row->hl != NULL && row->rsize > 0 && row->hl[row->rsize - 1] == HL_MLCOMMENT &&
	    (row->rsize < 2 || row->render[row->rsize - 2] != '*' ||
	     row->render[row->rsize - 1] != '/')) {
		return 1;
	}
	return 0;
}

void UpdateSyntax(EditorConfig *editor, EditorRow *row)
{
	row->hl = static_cast<unsigned char *>(KiloRealloc(row->hl, static_cast<size_t>(row->rsize)));
	if (row->rsize > 0 && row->hl != NULL) {
		memset(row->hl, HL_NORMAL, static_cast<size_t>(row->rsize));
	}

	if (editor->syntax == NULL || row->hl == NULL) {
		return;
	}

	char **keywords = editor->syntax->keywords;
	char *scs = editor->syntax->singleline_comment_start;
	char *mcs = editor->syntax->multiline_comment_start;
	char *mce = editor->syntax->multiline_comment_end;
	char *p = row->render;
	int i = 0;

	while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
		++p;
		++i;
	}

	int prev_sep = 1;
	int in_string = 0;
	int in_comment = 0;

	if (row->idx > 0 && RowHasOpenComment(&editor->row[row->idx - 1])) {
		in_comment = 1;
	}

	while (*p != '\0') {
		if (prev_sep && *p == scs[0] && *(p + 1) == scs[1]) {
			memset(row->hl + i, HL_COMMENT, static_cast<size_t>(row->rsize - i));
			return;
		}

		if (in_comment) {
			row->hl[i] = HL_MLCOMMENT;
			if (*p == mce[0] && *(p + 1) == mce[1] && i + 1 < row->rsize) {
				row->hl[i + 1] = HL_MLCOMMENT;
				p += 2;
				i += 2;
				in_comment = 0;
				prev_sep = 1;
				continue;
			}
			++p;
			++i;
			prev_sep = 0;
			continue;
		}

		if (*p == mcs[0] && *(p + 1) == mcs[1] && i + 1 < row->rsize) {
			row->hl[i] = HL_MLCOMMENT;
			row->hl[i + 1] = HL_MLCOMMENT;
			p += 2;
			i += 2;
			in_comment = 1;
			prev_sep = 0;
			continue;
		}

		if (editor->syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				if (*p == '\\' && *(p + 1) != '\0' && i + 1 < row->rsize) {
					row->hl[i + 1] = HL_STRING;
					p += 2;
					i += 2;
					prev_sep = 0;
					continue;
				}
				if (*p == in_string) {
					in_string = 0;
				}
				++p;
				++i;
				prev_sep = 0;
				continue;
			}

			if (*p == '"' || *p == '\'') {
				in_string = *p;
				row->hl[i] = HL_STRING;
				++p;
				++i;
				prev_sep = 0;
				continue;
			}
		}

		if (!isprint(static_cast<unsigned char>(*p))) {
			row->hl[i] = HL_NONPRINT;
			++p;
			++i;
			prev_sep = 0;
			continue;
		}

		if ((editor->syntax->flags & HL_HIGHLIGHT_NUMBERS) != 0 &&
		    ((isdigit(static_cast<unsigned char>(*p)) &&
		      (prev_sep || (i > 0 && row->hl[i - 1] == HL_NUMBER))) ||
		     (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER))) {
			row->hl[i] = HL_NUMBER;
			++p;
			++i;
			prev_sep = 0;
			continue;
		}

		if (prev_sep) {
			for (int j = 0; keywords[j] != NULL; ++j) {
				int klen = static_cast<int>(strlen(keywords[j]));
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) {
					--klen;
				}

				if (strncmp(p, keywords[j], static_cast<size_t>(klen)) == 0 &&
				    IsSeparator(*(p + klen))) {
					memset(row->hl + i, kw2 ? HL_KEYWORD2 : HL_KEYWORD1,
					       static_cast<size_t>(klen));
					p += klen;
					i += klen;
					prev_sep = 0;
					goto next_char;
				}
			}
		}

		prev_sep = IsSeparator(*p);
		++p;
		++i;

	next_char:
		;
	}

	int open_comment = RowHasOpenComment(row);
	if (row->hl_open_comment != open_comment && row->idx + 1 < editor->numrows) {
		UpdateSyntax(editor, &editor->row[row->idx + 1]);
	}
	row->hl_open_comment = open_comment;
}

void UpdateRow(EditorConfig *editor, EditorRow *row)
{
	int tabs = 0;
	for (int j = 0; j < row->size; ++j) {
		if (row->chars[j] == TAB) {
			++tabs;
		}
	}

	KiloFree(row->render);
	row->render =
		static_cast<char *>(KiloAlloc(static_cast<size_t>(row->size + tabs * 8 + 1)));
	if (row->render == NULL) {
		row->rsize = 0;
		return;
	}

	int idx = 0;
	for (int j = 0; j < row->size; ++j) {
		if (row->chars[j] == TAB) {
			row->render[idx++] = ' ';
			while ((idx + 1) % 8 != 0) {
				row->render[idx++] = ' ';
			}
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
	UpdateSyntax(editor, row);
}

void InsertRow(EditorConfig *editor, int at, const char *s, size_t len)
{
	if (at < 0 || at > editor->numrows) {
		return;
	}

	EditorRow *next = static_cast<EditorRow *>(
		KiloRealloc(editor->row, sizeof(EditorRow) * static_cast<size_t>(editor->numrows + 1)));
	if (next == NULL) {
		return;
	}
	editor->row = next;

	if (at != editor->numrows) {
		memmove(&editor->row[at + 1], &editor->row[at],
			sizeof(EditorRow) * static_cast<size_t>(editor->numrows - at));
		for (int j = at + 1; j <= editor->numrows; ++j) {
			editor->row[j].idx++;
		}
	}

	EditorRow *row = &editor->row[at];
	row->idx = at;
	row->size = static_cast<int>(len);
	row->chars = static_cast<char *>(KiloAlloc(len + 1));
	row->render = NULL;
	row->hl = NULL;
	row->rsize = 0;
	row->hl_open_comment = 0;
	if (row->chars == NULL) {
		row->size = 0;
		return;
	}
	memcpy(row->chars, s, len);
	row->chars[len] = '\0';
	UpdateRow(editor, row);
	++editor->numrows;
	++editor->dirty;
}

void FreeRow(EditorRow *row)
{
	KiloFree(row->chars);
	KiloFree(row->render);
	KiloFree(row->hl);
	row->chars = NULL;
	row->render = NULL;
	row->hl = NULL;
}

void DeleteRow(EditorConfig *editor, int at)
{
	if (at < 0 || at >= editor->numrows) {
		return;
	}

	FreeRow(&editor->row[at]);
	memmove(&editor->row[at], &editor->row[at + 1],
		sizeof(EditorRow) * static_cast<size_t>(editor->numrows - at - 1));
	for (int j = at; j < editor->numrows - 1; ++j) {
		editor->row[j].idx = j;
	}
	--editor->numrows;
	++editor->dirty;
}

void RowInsertChar(EditorConfig *editor, EditorRow *row, int at, int c)
{
	if (at < 0 || at > row->size) {
		at = row->size;
	}

	char *next = static_cast<char *>(KiloRealloc(row->chars, static_cast<size_t>(row->size + 2)));
	if (next == NULL) {
		return;
	}
	row->chars = next;
	memmove(&row->chars[at + 1], &row->chars[at], static_cast<size_t>(row->size - at + 1));
	row->size++;
	row->chars[at] = static_cast<char>(c);
	UpdateRow(editor, row);
	++editor->dirty;
}

void RowAppendString(EditorConfig *editor, EditorRow *row, const char *s, size_t len)
{
	char *next =
		static_cast<char *>(KiloRealloc(row->chars, static_cast<size_t>(row->size) + len + 1));
	if (next == NULL) {
		return;
	}
	row->chars = next;
	memcpy(row->chars + row->size, s, len);
	row->size += static_cast<int>(len);
	row->chars[row->size] = '\0';
	UpdateRow(editor, row);
	++editor->dirty;
}

void RowDeleteChar(EditorConfig *editor, EditorRow *row, int at)
{
	if (at < 0 || at >= row->size) {
		return;
	}
	memmove(&row->chars[at], &row->chars[at + 1], static_cast<size_t>(row->size - at));
	row->size--;
	UpdateRow(editor, row);
	++editor->dirty;
}

void SelectSyntaxHighlight(EditorConfig *editor, const char *filename)
{
	editor->syntax = NULL;
	for (size_t j = 0; j < HLDB_ENTRIES; ++j) {
		EditorSyntax *syntax = &HLDB[j];
		for (size_t i = 0; syntax->filematch[i] != NULL; ++i) {
			const char *pattern = syntax->filematch[i];
			const char *match = strstr(filename, pattern);
			if (match != NULL) {
				size_t patlen = strlen(pattern);
				if (pattern[0] != '.' || match[patlen] == '\0') {
					editor->syntax = syntax;
					for (int row = 0; row < editor->numrows; ++row) {
						UpdateSyntax(editor, &editor->row[row]);
					}
					return;
				}
			}
		}
	}
}

int Open(EditorConfig *editor, const char *filename)
{
	ClearEditor(editor);
	(void)snprintf(editor->filename, sizeof(editor->filename), "%s", filename);

	char fs_path[160];
	int rc = storage::ResolveManagedPath(filename, fs_path, sizeof(fs_path));
	if (rc != 0) {
		return rc;
	}

	struct fs_dirent entry = {};
	rc = fs_stat(fs_path, &entry);
	if (rc == -ENOENT) {
		SelectSyntaxHighlight(editor, editor->filename);
		return 0;
	}
	if (rc != 0) {
		return rc;
	}
	if (entry.type != FS_DIR_ENTRY_FILE) {
		return -EISDIR;
	}

	struct fs_file_t file;
	fs_file_t_init(&file);
	rc = fs_open(&file, fs_path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	char *line = static_cast<char *>(KiloAlloc(256));
	if (line == NULL) {
		(void)fs_close(&file);
		return -ENOMEM;
	}
	size_t line_cap = 256U;
	size_t line_len = 0U;
	char chunk[kReadChunkBytes];

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk));
		if (read_len < 0) {
			rc = static_cast<int>(read_len);
			break;
		}
		if (read_len == 0) {
			rc = 0;
			break;
		}

		for (ssize_t i = 0; i < read_len; ++i) {
			char ch = chunk[i];
			if (ch == '\n') {
				if (line_len > 0U && line[line_len - 1U] == '\r') {
					line_len--;
				}
				InsertRow(editor, editor->numrows, line, line_len);
				line_len = 0U;
				continue;
			}

			if (line_len + 1U >= line_cap) {
				size_t next_cap = line_cap * 2U;
				char *next = static_cast<char *>(KiloRealloc(line, next_cap));
				if (next == NULL) {
					rc = -ENOMEM;
					goto open_cleanup;
				}
				line = next;
				line_cap = next_cap;
			}

			line[line_len++] = ch;
			line[line_len] = '\0';
		}
	}

	if (rc == 0 && line_len > 0U) {
		InsertRow(editor, editor->numrows, line, line_len);
	}

open_cleanup:
	KiloFree(line);
	(void)fs_close(&file);
	if (rc != 0) {
		ClearEditor(editor);
		return rc;
	}

	editor->dirty = 0;
	SelectSyntaxHighlight(editor, editor->filename);
	return 0;
}

int Save(EditorConfig *editor)
{
	char fs_path[160];
	int rc = storage::ResolveManagedPath(editor->filename, fs_path, sizeof(fs_path));
	if (rc != 0) {
		SetStatusMessage(editor, "save failed: %d", rc);
		return rc;
	}

	rc = EnsureParentDirsForFsPath(fs_path);
	if (rc != 0) {
		SetStatusMessage(editor, "save failed: %d", rc);
		return rc;
	}

	struct fs_file_t file;
	fs_file_t_init(&file);
	rc = fs_open(&file, fs_path, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE);
	if (rc != 0) {
		SetStatusMessage(editor, "save failed: %d", rc);
		return rc;
	}

	size_t total_written = 0U;
	for (int i = 0; i < editor->numrows; ++i) {
		EditorRow *row = &editor->row[i];
		if (row->size > 0) {
			ssize_t written = fs_write(&file, row->chars, static_cast<size_t>(row->size));
			if (written < 0 || written != row->size) {
				rc = (written < 0) ? static_cast<int>(written) : -EIO;
				(void)fs_close(&file);
				SetStatusMessage(editor, "save failed: %d", rc);
				return rc;
			}
			total_written += static_cast<size_t>(written);
		}

		ssize_t newline_written = fs_write(&file, "\n", 1U);
		if (newline_written < 0 || newline_written != 1) {
			rc = (newline_written < 0) ? static_cast<int>(newline_written) : -EIO;
			(void)fs_close(&file);
			SetStatusMessage(editor, "save failed: %d", rc);
			return rc;
		}
		total_written += 1U;
	}

	(void)fs_close(&file);
	if (rc != 0) {
		SetStatusMessage(editor, "save failed: %d", rc);
		return rc;
	}

	editor->dirty = 0;
	SetStatusMessage(editor, "%u bytes written", static_cast<unsigned int>(total_written));
	return 0;
}

int SyntaxToColor(int hl)
{
	switch (hl) {
	case HL_COMMENT:
	case HL_MLCOMMENT:
		return 36;
	case HL_KEYWORD1:
		return 33;
	case HL_KEYWORD2:
		return 32;
	case HL_STRING:
		return 35;
	case HL_NUMBER:
		return 31;
	case HL_MATCH:
		return 34;
	default:
		return 37;
	}
}

void Scroll(EditorConfig *editor)
{
	if (editor->cy < 0) {
		editor->cy = 0;
	}
	if (editor->cy >= editor->screenrows) {
		editor->cy = editor->screenrows - 1;
	}

	int filerow = editor->rowoff + editor->cy;
	if (filerow < editor->rowoff) {
		editor->rowoff = filerow;
	}
	if (filerow >= editor->rowoff + editor->screenrows) {
		editor->rowoff = filerow - editor->screenrows + 1;
	}

	int filecol = editor->coloff + editor->cx;
	if (filecol < editor->coloff) {
		editor->coloff = filecol;
	}
	if (filecol >= editor->coloff + editor->screencols) {
		editor->coloff = filecol - editor->screencols + 1;
	}
}

void RefreshScreen(EditorConfig *editor)
{
	Scroll(editor);

	AppendBuffer ab = ABUF_INIT;
	Append(&ab, "\x1b[?25l", 6);
	Append(&ab, "\x1b[H", 3);

	for (int y = 0; y < editor->screenrows; ++y) {
		int filerow = editor->rowoff + y;
		if (filerow >= editor->numrows) {
			if (editor->numrows == 0 && y == editor->screenrows / 3) {
				char welcome[96];
				int welcome_len = snprintf(welcome, sizeof(welcome),
					"Kilo/Zephyr - Ctrl-S save | Ctrl-Q quit | Ctrl-F find");
				if (welcome_len > editor->screencols) {
					welcome_len = editor->screencols;
				}
				int padding = (editor->screencols - welcome_len) / 2;
				if (padding > 0) {
					Append(&ab, "~", 1);
					padding--;
				}
				while (padding-- > 0) {
					Append(&ab, " ", 1);
				}
				Append(&ab, welcome, welcome_len);
			} else {
				Append(&ab, "~", 1);
			}
			Append(&ab, "\x1b[0K\r\n", 6);
			continue;
		}

		EditorRow *row = &editor->row[filerow];
		int len = row->rsize - editor->coloff;
		int current_color = -1;
		if (len > 0) {
			if (len > editor->screencols) {
				len = editor->screencols;
			}
			char *c = row->render + editor->coloff;
			unsigned char *hl = row->hl + editor->coloff;
			for (int j = 0; j < len; ++j) {
				if (hl[j] == HL_NONPRINT) {
					char sym = c[j] <= 26 ? static_cast<char>('@' + c[j]) : '?';
					Append(&ab, "\x1b[7m", 4);
					Append(&ab, &sym, 1);
					Append(&ab, "\x1b[0m", 4);
					if (current_color != -1) {
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
						Append(&ab, buf, clen);
					}
				} else if (hl[j] == HL_NORMAL) {
					if (current_color != -1) {
						Append(&ab, "\x1b[39m", 5);
						current_color = -1;
					}
					Append(&ab, &c[j], 1);
				} else {
					int color = SyntaxToColor(hl[j]);
					if (color != current_color) {
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						current_color = color;
						Append(&ab, buf, clen);
					}
					Append(&ab, &c[j], 1);
				}
			}
		}
		Append(&ab, "\x1b[39m\x1b[0K\r\n", 11);
	}

	Append(&ab, "\x1b[0K\x1b[7m", 8);
	char status[96];
	char rstatus[32];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
		editor->filename[0] != '\0' ? editor->filename : "[No Name]", editor->numrows,
		editor->dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", editor->rowoff + editor->cy + 1,
		editor->numrows);
	if (len > editor->screencols) {
		len = editor->screencols;
	}
	Append(&ab, status, len);
	while (len < editor->screencols) {
		if (editor->screencols - len == rlen) {
			Append(&ab, rstatus, rlen);
			break;
		}
		Append(&ab, " ", 1);
		len++;
	}
	Append(&ab, "\x1b[0m\r\n", 6);

	Append(&ab, "\x1b[0K", 4);
	int msglen = static_cast<int>(strlen(editor->statusmsg));
	if (msglen > 0 && k_uptime_get() - editor->statusmsg_time < 5000) {
		if (msglen > editor->screencols) {
			msglen = editor->screencols;
		}
		Append(&ab, editor->statusmsg, msglen);
	}

	int cx = 1;
	int filerow = editor->rowoff + editor->cy;
	EditorRow *row = filerow >= editor->numrows ? NULL : &editor->row[filerow];
	if (row != NULL) {
		for (int j = editor->coloff; j < editor->coloff + editor->cx; ++j) {
			if (j < row->size && row->chars[j] == TAB) {
				cx += 7 - (cx % 8);
			}
			cx++;
		}
	}

	char cursor[32];
	int cursor_len = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH", editor->cy + 1, cx);
	Append(&ab, cursor, cursor_len);
	Append(&ab, "\x1b[?25h", 6);
	WriteAll(*editor, ab.data, static_cast<size_t>(ab.len));
	FreeAppendBuffer(&ab);
}

void MoveCursor(EditorConfig *editor, int key)
{
	int filerow = editor->rowoff + editor->cy;
	int filecol = editor->coloff + editor->cx;
	EditorRow *row = filerow >= editor->numrows ? NULL : &editor->row[filerow];

	switch (key) {
	case ARROW_LEFT:
		if (editor->cx == 0) {
			if (editor->coloff > 0) {
				editor->coloff--;
			} else if (filerow > 0) {
				editor->cy--;
				editor->cx = editor->row[filerow - 1].size;
				if (editor->cx > editor->screencols - 1) {
					editor->coloff = editor->cx - editor->screencols + 1;
					editor->cx = editor->screencols - 1;
				}
			}
		} else {
			editor->cx--;
		}
		break;
	case ARROW_RIGHT:
		if (row != NULL && filecol < row->size) {
			if (editor->cx == editor->screencols - 1) {
				editor->coloff++;
			} else {
				editor->cx++;
			}
		} else if (row != NULL && filecol == row->size) {
			editor->cx = 0;
			editor->coloff = 0;
			if (editor->cy == editor->screenrows - 1) {
				editor->rowoff++;
			} else {
				editor->cy++;
			}
		}
		break;
	case ARROW_UP:
		if (editor->cy == 0) {
			if (editor->rowoff > 0) {
				editor->rowoff--;
			}
		} else {
			editor->cy--;
		}
		break;
	case ARROW_DOWN:
		if (filerow < editor->numrows) {
			if (editor->cy == editor->screenrows - 1) {
				editor->rowoff++;
			} else {
				editor->cy++;
			}
		}
		break;
	default:
		break;
	}

	filerow = editor->rowoff + editor->cy;
	filecol = editor->coloff + editor->cx;
	row = filerow >= editor->numrows ? NULL : &editor->row[filerow];
	int rowlen = row != NULL ? row->size : 0;
	if (filecol > rowlen) {
		editor->cx -= filecol - rowlen;
		if (editor->cx < 0) {
			editor->coloff += editor->cx;
			editor->cx = 0;
		}
	}
}

void InsertChar(EditorConfig *editor, int c)
{
	int filerow = editor->rowoff + editor->cy;
	int filecol = editor->coloff + editor->cx;
	if (filerow >= editor->numrows) {
		while (editor->numrows <= filerow) {
			InsertRow(editor, editor->numrows, "", 0);
		}
	}
	RowInsertChar(editor, &editor->row[filerow], filecol, c);
	if (editor->cx == editor->screencols - 1) {
		editor->coloff++;
	} else {
		editor->cx++;
	}
}

void InsertNewline(EditorConfig *editor)
{
	int filerow = editor->rowoff + editor->cy;
	int filecol = editor->coloff + editor->cx;
	EditorRow *row = filerow >= editor->numrows ? NULL : &editor->row[filerow];

	if (row == NULL) {
		if (filerow == editor->numrows) {
			InsertRow(editor, filerow, "", 0);
		}
	} else {
		if (filecol > row->size) {
			filecol = row->size;
		}
		if (filecol == 0) {
			InsertRow(editor, filerow, "", 0);
		} else {
			InsertRow(editor, filerow + 1, row->chars + filecol,
				  static_cast<size_t>(row->size - filecol));
			row = &editor->row[filerow];
			row->chars[filecol] = '\0';
			row->size = filecol;
			UpdateRow(editor, row);
		}
	}

	if (editor->cy == editor->screenrows - 1) {
		editor->rowoff++;
	} else {
		editor->cy++;
	}
	editor->cx = 0;
	editor->coloff = 0;
}

void DeleteChar(EditorConfig *editor)
{
	int filerow = editor->rowoff + editor->cy;
	int filecol = editor->coloff + editor->cx;
	EditorRow *row = filerow >= editor->numrows ? NULL : &editor->row[filerow];

	if (row == NULL || (filecol == 0 && filerow == 0)) {
		return;
	}

	if (filecol == 0) {
		filecol = editor->row[filerow - 1].size;
		RowAppendString(editor, &editor->row[filerow - 1], row->chars,
				static_cast<size_t>(row->size));
		DeleteRow(editor, filerow);
		if (editor->cy == 0) {
			editor->rowoff--;
		} else {
			editor->cy--;
		}
		editor->cx = filecol;
		if (editor->cx >= editor->screencols) {
			int shift = editor->cx - editor->screencols + 1;
			editor->cx -= shift;
			editor->coloff += shift;
		}
	} else {
		RowDeleteChar(editor, row, filecol - 1);
		if (editor->cx == 0 && editor->coloff > 0) {
			editor->coloff--;
		} else {
			editor->cx--;
		}
	}
}

void Find(EditorConfig *editor)
{
	char query[kQueryLen + 1] = { 0 };
	int qlen = 0;
	int last_match = -1;
	int find_next = 0;
	int saved_hl_line = -1;
	unsigned char *saved_hl = NULL;
	int saved_cx = editor->cx;
	int saved_cy = editor->cy;
	int saved_coloff = editor->coloff;
	int saved_rowoff = editor->rowoff;

	while (true) {
		SetStatusMessage(editor, "Search: %s (ESC cancel, arrows navigate)", query);
		RefreshScreen(editor);

		int key = ReadKey(*editor);
		if (key < 0) {
			break;
		}

		if (saved_hl != NULL) {
			memcpy(editor->row[saved_hl_line].hl, saved_hl,
			       static_cast<size_t>(editor->row[saved_hl_line].rsize));
			KiloFree(saved_hl);
			saved_hl = NULL;
		}

		if (key == DEL_KEY || key == CTRL_H || key == BACKSPACE) {
			if (qlen > 0) {
				query[--qlen] = '\0';
			}
			last_match = -1;
		} else if (key == ESC || key == ENTER) {
			if (key == ESC) {
				editor->cx = saved_cx;
				editor->cy = saved_cy;
				editor->coloff = saved_coloff;
				editor->rowoff = saved_rowoff;
			}
			SetStatusMessage(editor, "");
			break;
		} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
			find_next = 1;
		} else if (key == ARROW_LEFT || key == ARROW_UP) {
			find_next = -1;
		} else if (isprint(key) && qlen < static_cast<int>(kQueryLen)) {
			query[qlen++] = static_cast<char>(key);
			query[qlen] = '\0';
			last_match = -1;
		}

		if (last_match == -1) {
			find_next = 1;
		}
		if (find_next == 0 || qlen == 0) {
			continue;
		}

		int current = last_match;
		for (int i = 0; i < editor->numrows; ++i) {
			current += find_next;
			if (current == -1) {
				current = editor->numrows - 1;
			} else if (current == editor->numrows) {
				current = 0;
			}

			char *match = strstr(editor->row[current].render, query);
			if (match != NULL) {
				int offset = static_cast<int>(match - editor->row[current].render);
				last_match = current;
				find_next = 0;
				if (editor->row[current].hl != NULL) {
					saved_hl_line = current;
					saved_hl = static_cast<unsigned char *>(
						KiloAlloc(static_cast<size_t>(editor->row[current].rsize)));
					if (saved_hl != NULL) {
						memcpy(saved_hl, editor->row[current].hl,
						       static_cast<size_t>(editor->row[current].rsize));
						memset(editor->row[current].hl + offset, HL_MATCH,
						       static_cast<size_t>(qlen));
					}
				}
				editor->cy = 0;
				editor->cx = offset;
				editor->rowoff = current;
				editor->coloff = 0;
				if (editor->cx > editor->screencols) {
					int diff = editor->cx - editor->screencols;
					editor->cx -= diff;
					editor->coloff += diff;
				}
				break;
			}
		}
	}

	KiloFree(saved_hl);
}

void ProcessKeypress(EditorConfig *editor)
{
	static int quit_times = kQuitTimes;
	int key = ReadKey(*editor);
	if (key < 0) {
		editor->last_error = key;
		editor->should_quit = true;
		return;
	}

	switch (key) {
	case ENTER:
		InsertNewline(editor);
		break;
	case CTRL_C:
		break;
	case CTRL_Q:
		if (editor->dirty && quit_times > 0) {
			SetStatusMessage(editor,
					 "WARNING: unsaved changes. Press Ctrl-Q %d more times to quit",
					 quit_times);
			quit_times--;
			return;
		}
		editor->should_quit = true;
		break;
	case CTRL_S:
		editor->last_error = Save(editor);
		break;
	case CTRL_F:
		Find(editor);
		break;
	case BACKSPACE:
	case CTRL_H:
	case DEL_KEY:
		if (key == DEL_KEY) {
			MoveCursor(editor, ARROW_RIGHT);
		}
		DeleteChar(editor);
		break;
	case PAGE_UP:
	case PAGE_DOWN:
		if (key == PAGE_UP) {
			editor->cy = 0;
		} else {
			editor->cy = editor->screenrows - 1;
		}
		for (int times = editor->screenrows; times-- > 0;) {
			MoveCursor(editor, key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		}
		break;
	case HOME_KEY:
		editor->cx = 0;
		editor->coloff = 0;
		break;
	case END_KEY: {
		int filerow = editor->rowoff + editor->cy;
		if (filerow < editor->numrows) {
			editor->cx = editor->row[filerow].size;
			if (editor->cx >= editor->screencols) {
				editor->coloff = editor->cx - editor->screencols + 1;
				editor->cx = editor->screencols - 1;
			} else {
				editor->coloff = 0;
			}
		}
		break;
	}
	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		MoveCursor(editor, key);
		break;
	case CTRL_L:
	case ESC:
		break;
	default:
		if (isprint(key) || key == TAB) {
			InsertChar(editor, key);
		}
		break;
	}

	quit_times = kQuitTimes;
}

void InitEditor(EditorConfig *editor, const Io &io, int rows, int cols)
{
	memset(editor, 0, sizeof(*editor));
	editor->io = io;
	editor->screenrows = rows > 3 ? rows - 2 : 22;
	editor->screencols = cols > 10 ? cols : 80;
	editor->statusmsg_time = 0;
	editor->last_error = 0;
}

void Cleanup(EditorConfig *editor)
{
	ClearEditor(editor);
}

} // namespace

int Run(const Io &io, const char *path, int screen_rows, int screen_cols)
{
	if (path == nullptr || path[0] == '\0' || io.read_char == nullptr || io.write == nullptr) {
		return -EINVAL;
	}

	EditorConfig editor;
	ResetKiloHeap();
	InitEditor(&editor, io, screen_rows, screen_cols);

	int rc = Open(&editor, path);
	if (rc != 0) {
		Cleanup(&editor);
		return rc;
	}

	SetStatusMessage(&editor, "HELP: Ctrl-S save | Ctrl-Q quit | Ctrl-F find");
	while (!editor.should_quit) {
		RefreshScreen(&editor);
		ProcessKeypress(&editor);
	}

	WriteAll(editor, "\x1b[2J\x1b[H\x1b[0m\x1b[?25h", 16);
	rc = editor.last_error;
	Cleanup(&editor);
	return rc;
}

bool GetHeapSnapshot(memory::HeapSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return false;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->available = true;
	snapshot->capacity_bytes = sizeof(g_kilo_heap_storage);

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats stats = {};
	k_mutex_lock(&g_kilo_heap_lock, K_FOREVER);
	int rc = sys_heap_runtime_stats_get(&g_kilo_heap, &stats);
	k_mutex_unlock(&g_kilo_heap_lock);
	if (rc != 0) {
		return false;
	}

	snapshot->free_bytes = stats.free_bytes;
	snapshot->allocated_bytes = stats.allocated_bytes;
	snapshot->peak_allocated_bytes = stats.max_allocated_bytes;
#else
	snapshot->free_bytes = 0U;
	snapshot->allocated_bytes = 0U;
	snapshot->peak_allocated_bytes = 0U;
#endif

	return true;
}

} // namespace fanctl::kilo