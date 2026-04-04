/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_WOLFSSH_USER_SETTINGS_H_
#define FAN_CONTROLLER_WOLFSSH_USER_SETTINGS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <wolfssl/wolfcrypt/types.h>

#define WS_NO_SIGNAL
#define WS_USE_TEST_BUFFERS
#define WOLFSSH_NO_NONBLOCKING
#define WOLFSSH_SHELL
#define DEFAULT_WINDOW_SZ (128 * 128)
#define DEFAULT_MAX_PACKET_SZ 4096

#ifdef __cplusplus
}
#endif

#endif
