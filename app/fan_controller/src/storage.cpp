/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree/fixed-partitions.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "common.hpp"
#include "generated/web_assets.hpp"

LOG_MODULE_REGISTER(fanctl_storage, LOG_LEVEL_INF);

namespace fanctl::storage {

namespace {

#define STORAGE_PARTITION storage_partition
#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(STORAGE_PARTITION)

constexpr const char *kMountPoint = "/lfs";
constexpr const char *kEtcRoot = "/lfs/etc";
constexpr const char *kEtcFanctlRoot = "/lfs/etc/fanctl";
constexpr const char *kEtcFanctlCurvesRoot = "/lfs/etc/fanctl/curves";
constexpr const char *kEtcSshRoot = "/lfs/etc/ssh";
constexpr const char *kBinRoot = "/lfs/bin";
constexpr const char *kDevRoot = "/lfs/dev";
constexpr const char *kProcRoot = "/lfs/proc";
constexpr const char *kRootHome = "/lfs/root";
constexpr const char *kRootSshRoot = "/lfs/root/.ssh";
constexpr const char *kRunRoot = "/lfs/run";
constexpr const char *kSrvRoot = "/lfs/srv";
constexpr const char *kWebRoot = "/lfs/srv/www";
constexpr const char *kUsrRoot = "/lfs/usr";
constexpr const char *kUsrBinRoot = "/lfs/usr/bin";
constexpr const char *kVarRoot = "/lfs/var";
constexpr const char *kVarLibRoot = "/lfs/var/lib";
constexpr const char *kVarLibFanctlRoot = "/lfs/var/lib/fanctl";
constexpr const char *kVarLogRoot = "/lfs/var/log";
constexpr const char *kHomeRoot = "/lfs/home";
constexpr const char *kMntRoot = "/lfs/mnt";
constexpr const char *kTmpRoot = "/lfs/tmp";
constexpr const char *kVersionFile = "/lfs/var/lib/fanctl/web.version";
constexpr const char *kConfigRelativePath = "/etc/fanctl/config.json";
constexpr const char *kHostnameFile = "/lfs/etc/hostname";
constexpr const char *kOsReleaseFile = "/lfs/etc/os-release";
constexpr const char *kMotdFile = "/lfs/etc/motd";
constexpr const char *kPasswdFile = "/lfs/etc/passwd";
constexpr const char *kGroupFile = "/lfs/etc/group";
constexpr const char *kShellsFile = "/lfs/etc/shells";
constexpr const char *kProcVersionFile = "/lfs/proc/version";
constexpr const char *kProcMountsFile = "/lfs/proc/mounts";

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(g_lfs_storage);

struct fs_mount_t g_lfs_mount = {
	.type = FS_LITTLEFS,
	.mnt_point = kMountPoint,
	.fs_data = &g_lfs_storage,
	.storage_dev = reinterpret_cast<void *>(STORAGE_PARTITION_ID),
};

bool g_initialized = false;

constexpr const char *kProtectedPaths[] = {
	"/bin",
	"/dev",
	"/etc",
	"/etc/fanctl",
	"/etc/fanctl/curves",
	"/etc/ssh",
	"/proc",
	"/root",
	"/root/.ssh",
	"/run",
	"/srv",
	"/srv/www",
	"/usr",
	"/usr/bin",
	"/var",
	"/var/lib",
	"/var/lib/fanctl",
};

int EnsureDir(const char *path)
{
	struct fs_dirent entry = {};
	int rc = fs_stat(path, &entry);

	if (rc == 0) {
		return (entry.type == FS_DIR_ENTRY_DIR) ? 0 : -ENOTDIR;
	}

	rc = fs_mkdir(path);

	if (rc == 0 || rc == -EEXIST) {
		return 0;
	}

	return rc;
}

int EnsureParentDirs(const char *path)
{
	char scratch[160];
	size_t len = strlen(path);

	if (len >= sizeof(scratch)) {
		return -ENOSPC;
	}

	(void)snprintf(scratch, sizeof(scratch), "%s", path);

	for (size_t i = strlen(kMountPoint) + 1U; scratch[i] != '\0'; ++i) {
		if (scratch[i] != '/') {
			continue;
		}

		scratch[i] = '\0';
		int rc = EnsureDir(scratch);
		scratch[i] = '/';
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int EnsureBaseLayout()
{
	static const char *const directories[] = {
		kBinRoot,         kDevRoot,         kEtcRoot,         kEtcFanctlRoot,   kEtcFanctlCurvesRoot,
		kEtcSshRoot,
		kHomeRoot,        kMntRoot,         kProcRoot,        kRootHome,         kRootSshRoot,
		kRunRoot,         kSrvRoot,         kWebRoot,         kTmpRoot,          kUsrRoot,
		kUsrBinRoot,      kVarRoot,         kVarLibRoot,      kVarLibFanctlRoot, kVarLogRoot,
	};

	for (const char *directory : directories) {
		int rc = EnsureDir(directory);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

bool FileExistsAbsolute(const char *path)
{
	struct fs_dirent entry = {};

	return path != nullptr && fs_stat(path, &entry) == 0;
}

int ReadFileAbsolute(const char *path, char *buffer, size_t buffer_len, size_t *out_len)
{
	if (path == nullptr || buffer == nullptr || buffer_len == 0U) {
		return -EINVAL;
	}

	struct fs_dirent entry = {};
	int rc = fs_stat(path, &entry);

	if (rc != 0) {
		return rc;
	}

	if (entry.type != FS_DIR_ENTRY_FILE) {
		return -EISDIR;
	}

	if (static_cast<size_t>(entry.size) + 1U > buffer_len) {
		return -ENOSPC;
	}

	struct fs_file_t file;
	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	ssize_t read_len = fs_read(&file, buffer, buffer_len - 1U);
	(void)fs_close(&file);
	if (read_len < 0) {
		return static_cast<int>(read_len);
	}

	buffer[read_len] = '\0';
	if (out_len != nullptr) {
		*out_len = static_cast<size_t>(read_len);
	}

	return 0;
}

int WriteFileAbsolute(const char *path, const char *data, size_t size)
{
	if (path == nullptr || data == nullptr) {
		return -EINVAL;
	}

	int rc = EnsureParentDirs(path);
	if (rc != 0) {
		return rc;
	}

	struct fs_file_t file;
	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE);
	if (rc != 0) {
		return rc;
	}

	ssize_t written = fs_write(&file, data, size);
	(void)fs_close(&file);
	if (written < 0) {
		return static_cast<int>(written);
	}

	return (static_cast<size_t>(written) == size) ? 0 : -EIO;
}

int EnsureSeedFile(const char *path, const char *content)
{
	if (FileExistsAbsolute(path)) {
		return 0;
	}

	return WriteFileAbsolute(path, content, strlen(content));
}

int WriteLinuxSeedFiles()
{
	char hostname[64];
	char os_release[256];
	char proc_version[160];
	char proc_mounts[160];

	(void)snprintf(hostname, sizeof(hostname), "%s\n", kDeviceHostname);
	(void)snprintf(os_release, sizeof(os_release),
		       "NAME=\"Fan Controller OS\"\n"
		       "ID=fanctl\n"
		       "PRETTY_NAME=\"Fan Controller OS on Zephyr\"\n"
		       "VERSION_ID=\"1\"\n"
		       "HOME_URL=\"http://%s/\"\n",
		       kDeviceHostname);
	(void)snprintf(proc_version, sizeof(proc_version),
		       "Zephyr device shell on ESP32-S3 (%s)\n", kDeviceHostname);
	(void)snprintf(proc_mounts, sizeof(proc_mounts), "littlefs / littlefs rw 0 0\n");

	int rc = EnsureSeedFile(kMotdFile, "ESP32-S3 fan controller\n");
	if (rc != 0) {
		return rc;
	}

	rc = EnsureSeedFile(kPasswdFile, "root:x:0:0:root:/root:/bin/sh\n");
	if (rc != 0) {
		return rc;
	}

	rc = EnsureSeedFile(kGroupFile, "root:x:0:\n");
	if (rc != 0) {
		return rc;
	}

	rc = EnsureSeedFile(kShellsFile, "/bin/sh\n");
	if (rc != 0) {
		return rc;
	}

	rc = WriteFileAbsolute(kHostnameFile, hostname, strlen(hostname));
	if (rc != 0) {
		return rc;
	}

	rc = WriteFileAbsolute(kOsReleaseFile, os_release, strlen(os_release));
	if (rc != 0) {
		return rc;
	}

	rc = WriteFileAbsolute(kProcVersionFile, proc_version, strlen(proc_version));
	if (rc != 0) {
		return rc;
	}

	return WriteFileAbsolute(kProcMountsFile, proc_mounts, strlen(proc_mounts));
}

bool NeedsAssetRefresh()
{
	char version[32];

	if (ReadFileAbsolute(kVersionFile, version, sizeof(version), nullptr) != 0) {
		return true;
	}

	return strcmp(version, kWebAssetsVersion) != 0;
}

int DeployAssets()
{
	int rc = EnsureDir(kWebRoot);
	if (rc != 0) {
		return rc;
	}

	if (!NeedsAssetRefresh()) {
		return 0;
	}

	for (size_t i = 0; i < kWebAssetCount; ++i) {
		char path[160];

		(void)snprintf(path, sizeof(path), "%s%s", kMountPoint, kWebAssets[i].path);
		rc = WriteFileAbsolute(path, reinterpret_cast<const char *>(kWebAssets[i].data),
				       kWebAssets[i].size);
		if (rc != 0) {
			return rc;
		}
	}

	return WriteFileAbsolute(kVersionFile, kWebAssetsVersion, strlen(kWebAssetsVersion));
}

const char *GuessContentType(const char *path)
{
	const char *extension = strrchr(path, '.');

	if (extension == nullptr) {
		return "application/octet-stream";
	}

	if (strcmp(extension, ".html") == 0) {
		return "text/html; charset=utf-8";
	}

	if (strcmp(extension, ".js") == 0) {
		return "application/javascript; charset=utf-8";
	}

	if (strcmp(extension, ".css") == 0) {
		return "text/css; charset=utf-8";
	}

	if (strcmp(extension, ".json") == 0) {
		return "application/json; charset=utf-8";
	}

	if (strcmp(extension, ".txt") == 0 || strcmp(extension, ".log") == 0 ||
	    strcmp(extension, ".pub") == 0) {
		return "text/plain; charset=utf-8";
	}

	if (strcmp(extension, ".pem") == 0) {
		return "application/x-pem-file";
	}

	return "application/octet-stream";
}

void AppendJsonEscaped(const char *src, char *dst, size_t dst_len, size_t *offset)
{
	for (size_t i = 0U; src[i] != '\0' && *offset + 2U < dst_len; ++i) {
		switch (src[i]) {
		case '"':
		case '\\':
			dst[(*offset)++] = '\\';
			dst[(*offset)++] = src[i];
			break;
		case '\n':
			dst[(*offset)++] = '\\';
			dst[(*offset)++] = 'n';
			break;
		case '\r':
			dst[(*offset)++] = '\\';
			dst[(*offset)++] = 'r';
			break;
		case '\t':
			dst[(*offset)++] = '\\';
			dst[(*offset)++] = 't';
			break;
		default:
			dst[(*offset)++] = src[i];
			break;
		}
	}
}

bool IsRootPath(const char *path)
{
	return path == nullptr || path[0] == '\0' || strcmp(path, "/") == 0;
}

bool IsProtectedPath(const char *path)
{
	if (IsRootPath(path)) {
		return true;
	}

	for (const char *protected_path : kProtectedPaths) {
		if (strcmp(path, protected_path) == 0) {
			return true;
		}
	}

	return false;
}

} // namespace

int Init()
{
	if (g_initialized) {
		return 0;
	}

	int rc = fs_mount(&g_lfs_mount);
	if (rc != 0) {
		LOG_ERR("littlefs mount failed: %d", rc);
		return rc;
	}

	rc = EnsureBaseLayout();
	if (rc != 0) {
		return rc;
	}

	rc = WriteLinuxSeedFiles();
	if (rc != 0) {
		LOG_ERR("linux seed layout failed: %d", rc);
		return rc;
	}

	rc = DeployAssets();
	if (rc != 0) {
		LOG_ERR("asset deploy failed: %d", rc);
		return rc;
	}

	g_initialized = true;
	LOG_INF("LittleFS mounted at %s", kMountPoint);
	return 0;
}

int EarlyInit()
{
	return Init();
}

SYS_INIT(EarlyInit, APPLICATION, 0);

bool ResolveWebPath(const char *http_path, char *fs_path, size_t fs_path_len,
		    const char **content_type)
{
	const char *requested = http_path;

	if (requested == nullptr || fs_path == nullptr || content_type == nullptr) {
		return false;
	}

	if (strcmp(requested, "/") == 0) {
		requested = "/index.html";
	}

	if (strstr(requested, "..") != nullptr) {
		return false;
	}

	(void)snprintf(fs_path, fs_path_len, "%s/srv/www%s", kMountPoint, requested);
	*content_type = GuessContentType(fs_path);
	return true;
}

int ResolveManagedPath(const char *user_path, char *fs_path, size_t fs_path_len)
{
	if (fs_path == nullptr || fs_path_len == 0U) {
		return -EINVAL;
	}

	const char *clean = user_path;
	if (IsRootPath(clean)) {
		clean = "/";
	}

	if (strstr(clean, "..") != nullptr || strchr(clean, '\\') != nullptr) {
		return -EINVAL;
	}

	if (clean[0] != '/') {
		return snprintf(fs_path, fs_path_len, "%s/%s", kMountPoint, clean) <
			       static_cast<int>(fs_path_len)
			       ? 0
			       : -ENOSPC;
	}

	return snprintf(fs_path, fs_path_len, "%s%s", kMountPoint, clean) <
			       static_cast<int>(fs_path_len)
		       ? 0
		       : -ENOSPC;
}

int ListDirectoryJson(const char *user_path, char *json, size_t json_len)
{
	if (json == nullptr || json_len == 0U) {
		return -EINVAL;
	}

	char fs_path[160];
	int rc = ResolveManagedPath(user_path, fs_path, sizeof(fs_path));
	if (rc != 0) {
		return rc;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		return rc;
	}

	size_t offset = 0U;
	int written = snprintf(json, json_len, "{\"path\":\"%s\",\"entries\":[",
			       IsRootPath(user_path) ? "/" : user_path);
	if (written <= 0 || static_cast<size_t>(written) >= json_len) {
		(void)fs_closedir(&dir);
		return -ENOSPC;
	}
	offset = static_cast<size_t>(written);

	bool first = true;
	while (true) {
		struct fs_dirent entry = {};
		rc = fs_readdir(&dir, &entry);
		if (rc != 0) {
			(void)fs_closedir(&dir);
			return rc;
		}

		if (entry.name[0] == '\0') {
			break;
		}

		char item_path[192];
		if (IsRootPath(user_path)) {
			(void)snprintf(item_path, sizeof(item_path), "/%s", entry.name);
		} else {
			(void)snprintf(item_path, sizeof(item_path), "%s/%s", user_path, entry.name);
		}

		written = snprintf(json + offset, json_len - offset, "%s{\"name\":\"",
				   first ? "" : ",");
		if (written <= 0 || static_cast<size_t>(written) >= json_len - offset) {
			(void)fs_closedir(&dir);
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
		AppendJsonEscaped(entry.name, json, json_len, &offset);

		written = snprintf(json + offset, json_len - offset, "\",\"path\":\"");
		if (written <= 0 || static_cast<size_t>(written) >= json_len - offset) {
			(void)fs_closedir(&dir);
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
		AppendJsonEscaped(item_path, json, json_len, &offset);

		written = snprintf(json + offset, json_len - offset, "\",\"type\":\"%s\",\"size\":%u}",
				   entry.type == FS_DIR_ENTRY_DIR ? "dir" : "file",
				   static_cast<unsigned int>(entry.size));
		if (written <= 0 || static_cast<size_t>(written) >= json_len - offset) {
			(void)fs_closedir(&dir);
			return -ENOSPC;
		}
		offset += static_cast<size_t>(written);
		first = false;
	}

	(void)fs_closedir(&dir);
	written = snprintf(json + offset, json_len - offset, "]}");
	return (written > 0 && static_cast<size_t>(written) < json_len - offset) ? 0 : -ENOSPC;
}

int ReadTextFile(const char *user_path, char *content, size_t content_len, size_t *out_len)
{
	char fs_path[160];
	int rc = ResolveManagedPath(user_path, fs_path, sizeof(fs_path));

	if (rc != 0) {
		return rc;
	}

	return ReadFileAbsolute(fs_path, content, content_len, out_len);
}

int WriteTextFile(const char *user_path, const char *content, size_t content_len)
{
	if (IsRootPath(user_path)) {
		return -EINVAL;
	}

	char fs_path[160];
	int rc = ResolveManagedPath(user_path, fs_path, sizeof(fs_path));

	if (rc != 0) {
		return rc;
	}

	return WriteFileAbsolute(fs_path, content, content_len);
}

int MakeDirectory(const char *user_path)
{
	if (IsRootPath(user_path)) {
		return 0;
	}

	char fs_path[160];
	int rc = ResolveManagedPath(user_path, fs_path, sizeof(fs_path));

	if (rc != 0) {
		return rc;
	}

	return EnsureDir(fs_path);
}

int DeletePath(const char *user_path)
{
	if (IsProtectedPath(user_path)) {
		return -EPERM;
	}

	char fs_path[160];
	int rc = ResolveManagedPath(user_path, fs_path, sizeof(fs_path));

	if (rc != 0) {
		return rc;
	}

	return fs_unlink(fs_path);
}

bool PathExists(const char *user_path)
{
	char fs_path[160];

	if (ResolveManagedPath(user_path, fs_path, sizeof(fs_path)) != 0) {
		return false;
	}

	return FileExistsAbsolute(fs_path);
}

const char *GetConfigPath()
{
	return kConfigRelativePath;
}

const char *GetMountPoint()
{
	return kMountPoint;
}

} // namespace fanctl::storage
