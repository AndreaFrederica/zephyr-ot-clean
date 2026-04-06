/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "memory_domains.hpp"

#include <string.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>

#include "http_common.hpp"

namespace fanctl::memory {

namespace {

EXT_RAM_BSS_ATTR char g_http_recv_buffers[http::kWorkerCount][http::kRecvBufferSize];
EXT_RAM_BSS_ATTR char g_http_scratch_buffers[http::kWorkerCount][http::kLargeBufferSize];
struct k_sem g_http_buffer_sem;
bool g_http_buffers_ready = false;
size_t g_http_peak_allocated_bytes = 0U;
size_t g_http_peak_slots_in_use = 0U;
bool g_http_slots_in_use[http::kWorkerCount];
struct k_spinlock g_http_lock;

size_t BytesPerSlot()
{
	return sizeof(g_http_recv_buffers[0]) + sizeof(g_http_scratch_buffers[0]);
}

void EnsureInitialized()
{
	if (g_http_buffers_ready) {
		return;
	}

	memset(g_http_slots_in_use, 0, sizeof(g_http_slots_in_use));
	k_sem_init(&g_http_buffer_sem, static_cast<unsigned int>(http::kWorkerCount),
		   static_cast<unsigned int>(http::kWorkerCount));
	g_http_buffers_ready = true;
}

} // namespace

void Init()
{
	EnsureInitialized();
}

bool AcquireHttpBufferSet(HttpBufferSet *buffers, k_timeout_t timeout)
{
	EnsureInitialized();
	if (buffers == nullptr) {
		return false;
	}

	if (k_sem_take(&g_http_buffer_sem, timeout) != 0) {
		memset(buffers, 0, sizeof(*buffers));
		return false;
	}

	k_spinlock_key_t key = k_spin_lock(&g_http_lock);
	size_t slot = http::kWorkerCount;
	size_t in_use_slots = 0U;
	for (size_t i = 0U; i < http::kWorkerCount; ++i) {
		if (g_http_slots_in_use[i]) {
			++in_use_slots;
			continue;
		}

		g_http_slots_in_use[i] = true;
		slot = i;
		++in_use_slots;
		break;
	}
	if (in_use_slots > g_http_peak_slots_in_use) {
		g_http_peak_slots_in_use = in_use_slots;
	}
	k_spin_unlock(&g_http_lock, key);

	if (slot >= http::kWorkerCount) {
		k_sem_give(&g_http_buffer_sem);
		memset(buffers, 0, sizeof(*buffers));
		return false;
	}

	buffers->slot = slot;
	buffers->recv_buffer = g_http_recv_buffers[slot];
	buffers->recv_capacity = sizeof(g_http_recv_buffers[slot]);
	buffers->scratch_buffer = g_http_scratch_buffers[slot];
	buffers->scratch_capacity = sizeof(g_http_scratch_buffers[slot]);

	size_t in_use_bytes = in_use_slots * BytesPerSlot();
	if (in_use_bytes > g_http_peak_allocated_bytes) {
		g_http_peak_allocated_bytes = in_use_bytes;
	}

	return true;
}

void ReleaseHttpBufferSet(HttpBufferSet *buffers)
{
	if (buffers == nullptr) {
		return;
	}

	EnsureInitialized();
	if (buffers->slot < http::kWorkerCount) {
		k_spinlock_key_t key = k_spin_lock(&g_http_lock);
		g_http_slots_in_use[buffers->slot] = false;
		k_spin_unlock(&g_http_lock, key);
	}
	memset(buffers, 0, sizeof(*buffers));
	k_sem_give(&g_http_buffer_sem);
}

bool GetHttpHeapSnapshot(HeapSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return false;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	EnsureInitialized();
	snapshot->available = true;
	snapshot->capacity_bytes = http::kWorkerCount * BytesPerSlot();
	snapshot->free_bytes = static_cast<size_t>(k_sem_count_get(&g_http_buffer_sem)) * BytesPerSlot();
	snapshot->allocated_bytes = snapshot->capacity_bytes - snapshot->free_bytes;
	snapshot->free_bytes = snapshot->capacity_bytes - snapshot->allocated_bytes;
	snapshot->peak_allocated_bytes = g_http_peak_allocated_bytes;

	return true;
}

bool GetPsramHeapSnapshot(HeapSnapshot *snapshot)
{
	if (snapshot == nullptr) {
		return false;
	}

	memset(snapshot, 0, sizeof(*snapshot));

	// 获取 PSRAM 堆信息 (使用 ESP-IDF heap_caps API)
	size_t total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	size_t allocated = total_size - free_size;

	if (total_size == 0) {
		// PSRAM 未启用或未初始化
		return false;
	}

	snapshot->available = true;
	snapshot->capacity_bytes = total_size;
	snapshot->free_bytes = free_size;
	snapshot->allocated_bytes = allocated;
	snapshot->peak_allocated_bytes = 0;  // ESP-IDF 不直接提供峰值统计

	return true;
}

} // namespace fanctl::memory
