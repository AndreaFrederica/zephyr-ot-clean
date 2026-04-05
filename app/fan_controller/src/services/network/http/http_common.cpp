/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http_common.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>

namespace fanctl::http {

namespace {

int DecodeHexNibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + c - 'A';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + c - 'a';
	}
	return -1;
}

bool DecodeUrlComponent(const char *src, size_t src_len, char *dst, size_t dst_len)
{
	size_t pos = 0U;

	for (size_t i = 0U; i < src_len && pos + 1U < dst_len; ++i) {
		if (src[i] == '+') {
			dst[pos++] = ' ';
			continue;
		}

		if (src[i] == '%' && i + 2U < src_len) {
			int hi = DecodeHexNibble(src[i + 1]);
			int lo = DecodeHexNibble(src[i + 2]);

			if (hi < 0 || lo < 0) {
				return false;
			}

			dst[pos++] = static_cast<char>((hi << 4) | lo);
			i += 2U;
			continue;
		}

		dst[pos++] = src[i];
	}

	dst[pos] = '\0';
	return true;
}

} // namespace

int SendAll(int client, const char *data, size_t len)
{
	size_t offset = 0U;

	while (offset < len) {
		int rc = zsock_send(client, data + offset, len - offset, 0);
		if (rc <= 0) {
			return (rc < 0) ? rc : -EIO;
		}
		offset += static_cast<size_t>(rc);
	}

	return 0;
}

int SendResponseSized(int client, const char *status, const char *content_type, const char *body,
		      size_t body_len)
{
	char header[256];
	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 %s\r\n"
				  "Content-Type: %s\r\n"
				  "Content-Length: %u\r\n"
				  "Connection: close\r\n\r\n",
				  status, content_type, static_cast<unsigned int>(body_len));

	if (header_len <= 0 || header_len >= static_cast<int>(sizeof(header))) {
		return -ENOMEM;
	}

	int rc = SendAll(client, header, static_cast<size_t>(header_len));
	if (rc != 0) {
		return rc;
	}

	return SendAll(client, body, body_len);
}

int SendResponse(int client, const char *status, const char *content_type, const char *body)
{
	return SendResponseSized(client, status, content_type, body, strlen(body));
}

int SendJsonResult(int client, bool ok, const char *message)
{
	char response[256];
	char escaped[128];

	// 转义 message 中的特殊字符，防止 JSON 注入
	JsonEscape(escaped, sizeof(escaped), message);

	int written = snprintf(response, sizeof(response), "{\"ok\":%s,\"message\":\"%s\"}",
			       ok ? "true" : "false", escaped);

	if (written <= 0 || written >= static_cast<int>(sizeof(response))) {
		return -ENOMEM;
	}

	return SendResponse(client, ok ? "200 OK" : "400 Bad Request", "application/json", response);
}

int SendFileResponse(int client, const char *status, const char *content_type, const char *path)
{
	struct fs_dirent entry = {};
	struct fs_file_t file;
	char header[256];
	char chunk[512];
	int rc = fs_stat(path, &entry);

	if (rc != 0 || entry.type != FS_DIR_ENTRY_FILE) {
		return -ENOENT;
	}

	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 %s\r\n"
				  "Content-Type: %s\r\n"
				  "Content-Length: %u\r\n"
				  "Connection: close\r\n\r\n",
				  status, content_type, static_cast<unsigned int>(entry.size));
	if (header_len <= 0 || header_len >= static_cast<int>(sizeof(header))) {
		return -ENOMEM;
	}

	rc = SendAll(client, header, static_cast<size_t>(header_len));
	if (rc != 0) {
		return rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	while (true) {
		ssize_t read_len = fs_read(&file, chunk, sizeof(chunk));
		if (read_len < 0) {
			(void)fs_close(&file);
			return static_cast<int>(read_len);
		}
		if (read_len == 0) {
			break;
		}

		rc = SendAll(client, chunk, static_cast<size_t>(read_len));
		if (rc != 0) {
			(void)fs_close(&file);
			return rc;
		}
	}

	(void)fs_close(&file);
	return 0;
}

void JsonEscape(char *dst, size_t dst_len, const char *src)
{
	size_t out = 0U;

	for (size_t i = 0U; src[i] != '\0' && out + 2U < dst_len; ++i) {
		char c = src[i];

		if (c == '"' || c == '\\') {
			dst[out++] = '\\';
			dst[out++] = c;
		} else if (static_cast<unsigned char>(c) < 0x20U) {
			dst[out++] = '_';
		} else {
			dst[out++] = c;
		}
	}

	dst[out] = '\0';
}

bool CopyKvValue(const char *source, const char *key, char *out, size_t out_len)
{
	if (source == nullptr || key == nullptr || out == nullptr || out_len == 0U) {
		return false;
	}

	for (const char *cursor = source; *cursor != '\0';) {
		const char *pair_end = strchr(cursor, '&');
		size_t pair_len = (pair_end != nullptr) ? static_cast<size_t>(pair_end - cursor)
							: strlen(cursor);
		const char *eq = static_cast<const char *>(memchr(cursor, '=', pair_len));

		if (eq != nullptr) {
			size_t key_len = static_cast<size_t>(eq - cursor);
			if (strlen(key) == key_len && strncmp(cursor, key, key_len) == 0) {
				return DecodeUrlComponent(eq + 1, pair_len - key_len - 1U, out, out_len);
			}
		}

		cursor = (pair_end != nullptr) ? (pair_end + 1) : (cursor + pair_len);
		if (pair_end == nullptr) {
			break;
		}
	}

	return false;
}

void SplitTarget(char *target, char **path, char **query)
{
	*path = target;
	*query = nullptr;

	char *separator = strchr(target, '?');
	if (separator != nullptr) {
		*separator = '\0';
		*query = separator + 1;
	}
}

} // namespace fanctl::http
