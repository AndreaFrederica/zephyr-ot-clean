/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_STORAGE_HPP_
#define FAN_CONTROLLER_STORAGE_HPP_

#include <stddef.h>

namespace fanctl::storage {

int Init();
bool ResolveWebPath(const char *http_path, char *fs_path, size_t fs_path_len,
		    const char **content_type);
int ResolveManagedPath(const char *user_path, char *fs_path, size_t fs_path_len);
int ListDirectoryJson(const char *user_path, char *json, size_t json_len);
int ReadTextFile(const char *user_path, char *content, size_t content_len, size_t *out_len);
int WriteTextFile(const char *user_path, const char *content, size_t content_len);
int MakeDirectory(const char *user_path);
int DeletePath(const char *user_path);
int CopyPath(const char *source_path, const char *target_path);
int MovePath(const char *source_path, const char *target_path);
bool PathExists(const char *user_path);
const char *GetConfigPath();
const char *GetMountPoint();

} // namespace fanctl::storage

#endif
