/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_MEMORY_DOMAINS_HPP_
#define FAN_CONTROLLER_MEMORY_DOMAINS_HPP_

#include <stddef.h>

#include <zephyr/kernel.h>

namespace fanctl::memory {

struct HeapSnapshot {
	bool available;
	size_t capacity_bytes;
	size_t free_bytes;
	size_t allocated_bytes;
	size_t peak_allocated_bytes;
};

struct HttpBufferSet {
	size_t slot;
	char *recv_buffer;
	size_t recv_capacity;
	char *scratch_buffer;
	size_t scratch_capacity;
};

void Init();
bool AcquireHttpBufferSet(HttpBufferSet *buffers, k_timeout_t timeout = K_NO_WAIT);
void ReleaseHttpBufferSet(HttpBufferSet *buffers);
bool GetHttpHeapSnapshot(HeapSnapshot *snapshot);
bool GetPsramHeapSnapshot(HeapSnapshot *snapshot);

} // namespace fanctl::memory

#endif
