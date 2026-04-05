/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ssh_server.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>

#include "command_session.hpp"
#include "core/common.hpp"
#include "storage.hpp"

LOG_MODULE_REGISTER(fanctl_ssh, LOG_LEVEL_INF);

namespace fanctl {

namespace {

K_THREAD_STACK_DEFINE(g_ssh_server_stack, kSshStackSize);

constexpr const char *kPemBegin = "-----BEGIN EC PRIVATE KEY-----\n";
constexpr const char *kPemEnd = "-----END EC PRIVATE KEY-----\n";

struct ConnectionContext {
	explicit ConnectionContext(const ServiceContext &services, const settings::SshConfig &ssh_config)
		: session(services), config(ssh_config),
		  shell_requested(false), exec_requested(false), reboot_requested(false)
	{
		exec_command[0] = '\0';
	}

	CommandSession session;
	settings::SshConfig config;
	bool shell_requested;
	bool exec_requested;
	bool reboot_requested;
	char exec_command[256];
};

int SendStreamAll(WOLFSSH *ssh, const char *data, size_t len)
{
	size_t offset = 0U;
	while (offset < len) {
		int written = wolfSSH_stream_send(
			ssh, reinterpret_cast<byte *>(const_cast<char *>(data + offset)),
			static_cast<word32>(len - offset));
		if (written <= 0) {
			return written;
		}
		offset += static_cast<size_t>(written);
	}

	return 0;
}

void SessionWriter(void *ctx, const char *text, size_t len)
{
	if (ctx == nullptr || text == nullptr || len == 0U) {
		return;
	}

	(void)SendStreamAll(static_cast<WOLFSSH *>(ctx), text, len);
}

int ReadCommandLine(WOLFSSH *ssh, char *buffer, size_t buffer_len, CommandSession *session)
{
	if (ssh == nullptr || buffer == nullptr || buffer_len < 2U) {
		return -EINVAL;
	}

	size_t offset = 0U;
	buffer[0] = '\0';

	while (offset + 1U < buffer_len) {
		byte ch = 0;
		int rc = wolfSSH_stream_read(ssh, &ch, 1);
		if (rc <= 0) {
			return rc == 0 ? -EIO : rc;
		}

		if (ch == '\r') {
			(void)SendStreamAll(ssh, "\r\n", 2);
			break;
		}

		if (ch == '\n') {
			break;
		}

		if (ch == 0x7fU || ch == 0x08U) {
			if (offset > 0U) {
				--offset;
				buffer[offset] = '\0';
				(void)SendStreamAll(ssh, "\b \b", 3);
			}
			continue;
		}

		if (ch == '\t' && session != nullptr) {
			// Handle Tab completion
			buffer[offset] = '\0';
			char completion[64];
			int complete_rc = session->Complete(buffer, SessionWriter, ssh, completion,
							    sizeof(completion));
			if (complete_rc > 0 && completion[0] != '\0') {
				// Apply the completion
				size_t comp_len = strlen(completion);
				if (offset + comp_len < buffer_len - 1) {
					memcpy(buffer + offset, completion, comp_len + 1);
					offset += comp_len;
					(void)SendStreamAll(ssh, completion, comp_len);
				}
			}
			continue;
		}

		buffer[offset++] = static_cast<char>(ch);
		buffer[offset] = '\0';
		(void)SendStreamAll(ssh, reinterpret_cast<const char *>(&ch), 1);
	}

	buffer[offset] = '\0';
	return static_cast<int>(offset);
}

bool MatchAuthorizedKeyLine(const char *line, const byte *public_key, word32 public_key_sz)
{
	if (line == nullptr || public_key == nullptr || public_key_sz == 0U) {
		return false;
	}

	while (*line == ' ' || *line == '\t') {
		++line;
	}

	if (*line == '\0' || *line == '#') {
		return false;
	}

	char scratch[512];
	(void)snprintf(scratch, sizeof(scratch), "%s", line);

	char *save = nullptr;
	char *type = strtok_r(scratch, " \t", &save);
	char *base64_key = strtok_r(nullptr, " \t", &save);

	if (type == nullptr || base64_key == nullptr) {
		return false;
	}

	if (strcmp(type, "ssh-ed25519") != 0 && strcmp(type, "ecdsa-sha2-nistp256") != 0 &&
	    strcmp(type, "ssh-rsa") != 0) {
		return false;
	}

	byte decoded[512];
	word32 decoded_len = sizeof(decoded);
	if (Base64_Decode(reinterpret_cast<byte *>(base64_key), strlen(base64_key), decoded,
			  &decoded_len) != 0) {
		return false;
	}

	return decoded_len == public_key_sz && memcmp(decoded, public_key, public_key_sz) == 0;
}

bool AuthorizedKeyMatches(const settings::SshConfig &config, const byte *public_key,
			  word32 public_key_sz)
{
	char content[2048];
	size_t content_len = 0U;
	if (storage::ReadTextFile(config.authorized_keys_path, content, sizeof(content), &content_len) !=
	    0) {
		return false;
	}

	char *save = nullptr;
	for (char *line = strtok_r(content, "\r\n", &save); line != nullptr;
	     line = strtok_r(nullptr, "\r\n", &save)) {
		if (MatchAuthorizedKeyLine(line, public_key, public_key_sz)) {
			return true;
		}
	}

	return false;
}

int UserAuthCallback(byte auth_type, WS_UserAuthData *auth_data, void *ctx)
{
	if (auth_data == nullptr || ctx == nullptr) {
		return WOLFSSH_USERAUTH_FAILURE;
	}

	const settings::SshConfig &config = static_cast<ConnectionContext *>(ctx)->config;

	char username[32];
	size_t username_len = MIN(static_cast<size_t>(auth_data->usernameSz), sizeof(username) - 1U);
	memcpy(username, auth_data->username, username_len);
	username[username_len] = '\0';

	if (strcmp(username, config.username) != 0) {
		return WOLFSSH_USERAUTH_INVALID_USER;
	}

	if (auth_type == WOLFSSH_USERAUTH_PASSWORD) {
		if (!config.allow_password_auth) {
			return WOLFSSH_USERAUTH_REJECTED;
		}

		char password[64];
		size_t password_len =
			MIN(static_cast<size_t>(auth_data->sf.password.passwordSz), sizeof(password) - 1U);
		memcpy(password, auth_data->sf.password.password, password_len);
		password[password_len] = '\0';
		return strcmp(password, config.password) == 0 ? WOLFSSH_USERAUTH_SUCCESS
							      : WOLFSSH_USERAUTH_INVALID_PASSWORD;
	}

	if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY) {
		if (!config.allow_public_key_auth) {
			return WOLFSSH_USERAUTH_REJECTED;
		}

		return AuthorizedKeyMatches(config, auth_data->sf.publicKey.publicKey,
					    auth_data->sf.publicKey.publicKeySz)
			       ? WOLFSSH_USERAUTH_SUCCESS
			       : WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
	}

	return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

int UserAuthTypesCallback(WOLFSSH *ssh, void *ctx)
{
	ARG_UNUSED(ssh);

	if (ctx == nullptr) {
		return 0;
	}

	const settings::SshConfig &config = static_cast<ConnectionContext *>(ctx)->config;
	int types = 0;
	if (config.allow_password_auth) {
		types |= WOLFSSH_USERAUTH_PASSWORD;
	}
	if (config.allow_public_key_auth) {
		types |= WOLFSSH_USERAUTH_PUBLICKEY;
	}
	return types;
}

int ShellRequestCallback(WOLFSSH_CHANNEL *channel, void *ctx)
{
	ARG_UNUSED(channel);

	if (ctx == nullptr) {
		return -1;
	}

	static_cast<ConnectionContext *>(ctx)->shell_requested = true;
	return 0;
}

int ExecRequestCallback(WOLFSSH_CHANNEL *channel, void *ctx)
{
	if (channel == nullptr || ctx == nullptr) {
		return -1;
	}

	ConnectionContext *connection = static_cast<ConnectionContext *>(ctx);
	connection->exec_requested = true;
	const char *command = wolfSSH_ChannelGetSessionCommand(channel);
	(void)snprintf(connection->exec_command, sizeof(connection->exec_command), "%s",
		       command != nullptr ? command : "");
	return 0;
}

} // namespace

SshServer::SshServer(const ServiceContext &services)
	: fan_controller_(*services.fan_controller), wifi_manager_(*services.wifi_manager),
	  host_control_(*services.host_control), config_{}, ctx_(nullptr), enabled_(false)
{
}

int SshServer::EnsureHostKey()
{
	char existing[1024];
	size_t existing_len = 0U;
	if (storage::ReadTextFile(config_.host_key_path, existing, sizeof(existing), &existing_len) == 0 &&
	    existing_len > 0U) {
		return 0;
	}

	WC_RNG rng;
	ecc_key key;
	byte *der = nullptr;
	byte *base64 = nullptr;
	char *pem = nullptr;

	int rc = wc_InitRng(&rng);
	if (rc != 0) {
		LOG_ERR("ssh rng init failed: %d", rc);
		return -EIO;
	}

	rc = wc_ecc_init(&key);
	if (rc != 0) {
		LOG_ERR("ssh ecc init failed: %d", rc);
		wc_FreeRng(&rng);
		return -EIO;
	}

	rc = wc_ecc_make_key_ex(&rng, 32, &key, ECC_SECP256R1);
	if (rc != 0) {
		wc_ecc_free(&key);
		wc_FreeRng(&rng);
		LOG_ERR("ssh host key generation failed: %d", rc);
		return -EIO;
	}

	int der_len = wc_EccPrivateKeyToDer(&key, nullptr, 0);
	if (der_len <= 0) {
		wc_ecc_free(&key);
		wc_FreeRng(&rng);
		LOG_ERR("ssh host key der size failed: %d", der_len);
		return -EIO;
	}

	der = static_cast<byte *>(malloc(static_cast<size_t>(der_len)));
	if (der == nullptr) {
		wc_ecc_free(&key);
		wc_FreeRng(&rng);
		return -ENOMEM;
	}

	rc = wc_EccPrivateKeyToDer(&key, der, static_cast<word32>(der_len));

	wc_ecc_free(&key);
	wc_FreeRng(&rng);

	if (rc <= 0) {
		free(der);
		LOG_ERR("ssh host key generation failed: %d", rc);
		return -EIO;
	}

	word32 base64_len = static_cast<word32>(((static_cast<word32>(rc) + 2U) / 3U) * 4U + 4U);
	base64 = static_cast<byte *>(malloc(base64_len));
	if (base64 == nullptr) {
		free(der);
		return -ENOMEM;
	}

	rc = Base64_Encode_NoNl(der, static_cast<word32>(rc), base64, &base64_len);
	free(der);
	if (rc != 0) {
		free(base64);
		LOG_ERR("ssh host key base64 encode failed: %d", rc);
		return -EIO;
	}

	size_t pem_capacity = strlen(kPemBegin) + strlen(kPemEnd) + static_cast<size_t>(base64_len) +
			      static_cast<size_t>((base64_len + 63U) / 64U) + 1U;
	pem = static_cast<char *>(malloc(pem_capacity));
	if (pem == nullptr) {
		free(base64);
		return -ENOMEM;
	}

	size_t offset = 0U;
	int written = snprintf(pem + offset, pem_capacity - offset, "%s", kPemBegin);
	if (written <= 0 || static_cast<size_t>(written) >= pem_capacity - offset) {
		free(base64);
		free(pem);
		return -ENOSPC;
	}
	offset += static_cast<size_t>(written);

	for (word32 i = 0; i < base64_len; i += 64U) {
		word32 chunk_len = MIN(base64_len - i, 64U);
		if (offset + chunk_len + 1U >= pem_capacity) {
			free(base64);
			free(pem);
			return -ENOSPC;
		}

		memcpy(pem + offset, base64 + i, chunk_len);
		offset += chunk_len;
		pem[offset++] = '\n';
	}
	free(base64);

	written = snprintf(pem + offset, pem_capacity - offset, "%s", kPemEnd);
	if (written <= 0 || static_cast<size_t>(written) >= pem_capacity - offset) {
		free(pem);
		return -ENOSPC;
	}
	offset += static_cast<size_t>(written);

	rc = storage::WriteTextFile(config_.host_key_path, pem, offset);
	free(pem);
	return rc;
}

int SshServer::SetupContext()
{
	if (ctx_ != nullptr) {
		return 0;
	}

	int ws_rc = wolfSSH_Init();
	if (ws_rc != WS_SUCCESS) {
		LOG_ERR("wolfSSH_Init failed: %d", ws_rc);
		return -EIO;
	}

	ctx_ = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
	if (ctx_ == nullptr) {
		return -ENOMEM;
	}

	wolfSSH_SetUserAuth(ctx_, UserAuthCallback);
	wolfSSH_SetUserAuthTypes(ctx_, UserAuthTypesCallback);
	wolfSSH_CTX_SetChannelReqShellCb(ctx_, ShellRequestCallback);
	wolfSSH_CTX_SetChannelReqExecCb(ctx_, ExecRequestCallback);
	(void)wolfSSH_CTX_SetBanner(ctx_, "ESP32-S3 fan controller SSH service\r\n");
	(void)wolfSSH_CTX_SetWindowPacketSize(ctx_, DEFAULT_WINDOW_SZ, DEFAULT_MAX_PACKET_SZ);

	for (int attempt = 0; attempt < 2; ++attempt) {
		char host_key[1024];
		byte host_key_der[512];
		size_t host_key_len = 0U;
		int rc = storage::ReadTextFile(config_.host_key_path, host_key, sizeof(host_key),
					       &host_key_len);
		if (rc != 0) {
			LOG_ERR("ssh host key read failed: %d", rc);
			wolfSSH_CTX_free(ctx_);
			ctx_ = nullptr;
			return rc;
		}

		int der_len = wc_KeyPemToDer(reinterpret_cast<const unsigned char *>(host_key),
						    static_cast<int>(host_key_len), host_key_der,
						    static_cast<int>(sizeof(host_key_der)), nullptr);
		if (der_len <= 0) {
			ws_rc = WS_BAD_FILE_E;
			LOG_WRN("ssh host key pem decode failed: %d (len=%u, attempt=%d)", der_len,
				static_cast<unsigned int>(host_key_len), attempt + 1);
		} else {
			ws_rc = wolfSSH_CTX_UsePrivateKey_buffer(ctx_, host_key_der,
							 static_cast<word32>(der_len),
							 WOLFSSH_FORMAT_ASN1);
		}
		if (ws_rc == WS_SUCCESS) {
			return 0;
		}

		if (der_len > 0) {
			LOG_WRN("ssh host key load failed: %d (der_len=%u, attempt=%d)", ws_rc,
				static_cast<unsigned int>(der_len), attempt + 1);
		}
		if (attempt == 0) {
			(void)storage::DeletePath(config_.host_key_path);
			rc = EnsureHostKey();
			if (rc != 0) {
				LOG_ERR("ssh host key regeneration failed: %d", rc);
				wolfSSH_CTX_free(ctx_);
				ctx_ = nullptr;
				return rc;
			}
			continue;
		}
	}

	wolfSSH_CTX_free(ctx_);
	ctx_ = nullptr;
	return -EIO;
}

int SshServer::Init()
{
	int rc = settings::LoadSshConfig(&config_);
	if (rc != 0) {
		LOG_ERR("ssh config load failed: %d", rc);
		return rc;
	}

	(void)snprintf(config_.host_key_path, sizeof(config_.host_key_path),
		       "/etc/ssh/ssh_host_ecdsa_key.pem");

	enabled_ = config_.enabled;
	if (!enabled_) {
		LOG_INF("SSH disabled in %s", settings::GetSshConfigRelativePath());
		return 0;
	}

	rc = EnsureHostKey();
	if (rc != 0) {
		LOG_ERR("ssh host key init failed: %d", rc);
		return rc;
	}

	rc = SetupContext();
	if (rc != 0) {
		LOG_ERR("ssh context setup failed: %d", rc);
		return rc;
	}

	return 0;
}

void SshServer::Start()
{
	if (!enabled_) {
		return;
	}

	k_thread_create(&thread_, g_ssh_server_stack, K_THREAD_STACK_SIZEOF(g_ssh_server_stack),
			ThreadEntry, this, nullptr,
			nullptr, 6, 0, K_NO_WAIT);
	k_thread_name_set(&thread_, "fanctl_ssh");

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	// 绑定到APP CPU (CPU 1)，与风扇控制隔离
	if (arch_num_cpus() > 1) {
		(void)k_thread_cpu_pin(&thread_, 1);
	}
#endif
}

bool SshServer::IsEnabled() const
{
	return enabled_;
}

int SshServer::GetListenPort() const
{
	return config_.listen_port;
}

void SshServer::ThreadEntry(void *ctx, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	static_cast<SshServer *>(ctx)->Run();
}

void SshServer::Run()
{
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(config_.listen_port));
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	int server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		LOG_ERR("ssh socket create failed");
		return;
	}

	if (zsock_bind(server, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		LOG_ERR("ssh bind failed");
		(void)zsock_close(server);
		return;
	}

	if (zsock_listen(server, 2) != 0) {
		LOG_ERR("ssh listen failed");
		(void)zsock_close(server);
		return;
	}

	LOG_INF("SSH server listening on port %d", config_.listen_port);

	while (true) {
		int client = zsock_accept(server, nullptr, nullptr);
		if (client < 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		(void)HandleClient(client);
		(void)zsock_close(client);
	}
}

int SshServer::HandleClient(int client)
{
	ServiceContext services = { &fan_controller_, &wifi_manager_, &host_control_ };
	ConnectionContext connection(services, config_);
	WOLFSSH *ssh = wolfSSH_new(ctx_);
	if (ssh == nullptr) {
		return -ENOMEM;
	}

	wolfSSH_set_fd(ssh, client);
	wolfSSH_SetUserAuthCtx(ssh, &connection);
	wolfSSH_SetChannelReqCtx(ssh, &connection);

	int rc = wolfSSH_accept(ssh);
	if (rc != WS_SUCCESS) {
		LOG_WRN("ssh accept failed: %d (%s)", rc, wolfSSH_ErrorToName(rc));
		wolfSSH_free(ssh);
		return -EIO;
	}

	if (!connection.shell_requested && !connection.exec_requested) {
		connection.shell_requested = true;
	}

	if (connection.exec_requested) {
		CommandSessionResult result = {};
		rc = connection.session.Execute(connection.exec_command, SessionWriter, ssh, &result);
		(void)wolfSSH_stream_exit(ssh, rc == 0 ? 0 : 1);
		if (result.reboot_requested) {
			connection.reboot_requested = true;
		}
	} else {
		char banner[192];
		(void)snprintf(banner, sizeof(banner),
			       "\r\nWelcome to %s.\r\nType 'help' for commands.\r\n", kDeviceHostname);
		(void)SendStreamAll(ssh, banner, strlen(banner));

		while (true) {
			char prompt[160];
			connection.session.BuildPrompt(prompt, sizeof(prompt));
			(void)SendStreamAll(ssh, prompt, strlen(prompt));

			char line[512];
			rc = ReadCommandLine(ssh, line, sizeof(line), &connection.session);
			if (rc < 0) {
				break;
			}

			CommandSessionResult result = {};
			(void)connection.session.Execute(line, SessionWriter, ssh, &result);
			if (result.reboot_requested) {
				connection.reboot_requested = true;
			}
			if (result.exit_requested) {
				break;
			}
		}

		(void)wolfSSH_stream_exit(ssh, 0);
	}

	(void)wolfSSH_shutdown(ssh);
	wolfSSH_free(ssh);

	if (connection.reboot_requested) {
		k_sleep(K_MSEC(100));
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

} // namespace fanctl
