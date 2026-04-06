# WiFi Crash Handoff Report

Date: 2026-04-06

Scope: analyze the long-run crash seen on the ESP32-S3 fan controller build without changing runtime code.

## 1. Executive Summary

The current evidence does not point to [`fatal_reboot.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/core/fatal_reboot.cpp) as the root cause. That file only reboots after Zephyr has already entered the fatal path.

The crash is most likely caused by a memory-management defect in the ESP32 Zephyr WiFi adapter layer, with one of these two mechanisms:

1. A long-run leak or fragmentation issue first causes WiFi allocations to fail.
2. A structural bug in the adapter corrupts heap metadata, and the corruption is later detected while the `esp_timer` thread is freeing or re-linking heap chunks.

The highest-probability root cause is not in application logic. The highest-probability root cause is in [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c), specifically its queue/task wrapper implementation.

## 2. Observed Runtime Timeline

Relevant serial log sequence from the failing run:

1. Boot completes.
2. AP mode starts, HTTP and SSH servers come up.
3. STA connects and receives DHCP lease.
4. NTP succeeds.
5. After about 4 minutes 42 seconds:
   `esp32_wifi_adapter: memory allocation failed`
6. After about 11 minutes 19 seconds:
   `esp32_wifi_adapter: memory allocation failed`
7. Immediately after the second allocation failure:
   `ZEPHYR FATAL ERROR 0: CPU exception on CPU 0`
8. Current thread at crash:
   `esp_timer`

Important implication:

- The adapter reports allocation failure before the fatal exception.
- The fatal exception happens later in a heap-management path, which is typical when memory corruption has already occurred earlier.

## 3. Address Mapping and Crash Signature

The failure log shows:

- PC: `0x42021591`
- A0 / return path near: `0x42021697`
- Current thread: `esp_timer`

Using the built image and map file in [`build_fan_controller/zephyr/zephyr.map`](D:/Projects/zephyr-ot-clean/build_fan_controller/zephyr/zephyr.map):

- `0x42021580` is [`chunk_set`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.h#L120)
- `0x42021644` is [`free_list_remove_bidx`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.c#L37)

The exact faulting instruction falls inside `chunk_set`, which writes heap metadata:

- [`heap.h`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.h#L120)

The return address is in the free-list removal path:

- [`heap.c`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.c#L37)

Interpretation:

- The immediate crash is in Zephyr heap bookkeeping, not in fan-controller application logic.
- This usually means one of:
  - freeing an invalid pointer
  - double-free
  - writing past an allocation boundary earlier
  - using the wrong object type/layout and later freeing it

## 4. Why `fatal_reboot.cpp` Is Not the Root Cause

[`fatal_reboot.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/core/fatal_reboot.cpp#L8) overrides `k_sys_fatal_error_handler()` and calls `sys_reboot()` after `LOG_PANIC()`.

That means:

- Zephyr has already detected a fatal CPU exception before this code runs.
- The file changes post-crash behavior only.
- It cannot explain the memory corruption or CPU exception itself.

The upstream fatal path is visible in [`fatal.c`](D:/Projects/zephyr-ot-clean/zephyr/kernel/fatal.c#L70).

## 5. Build and Memory Configuration Relevant to the Failure

From [`build_fan_controller/zephyr/.config`](D:/Projects/zephyr-ot-clean/build_fan_controller/zephyr/.config#L1085):

- `CONFIG_HEAP_MEM_POOL_SIZE=131072`

From [`build_fan_controller/zephyr/.config`](D:/Projects/zephyr-ot-clean/build_fan_controller/zephyr/.config#L1484):

- `CONFIG_ESP_WIFI_HEAP_SPIRAM=y`
- `CONFIG_ESP32_WIFI_AP_STA_MODE=y`
- `CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=16`
- `CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=64`
- `CONFIG_ESP32_WIFI_STATIC_TX_BUFFER_NUM=16`
- `CONFIG_ESP32_WIFI_CACHE_TX_BUFFER_NUM=16`

From [`build_fan_controller/zephyr/.config`](D:/Projects/zephyr-ot-clean/build_fan_controller/zephyr/.config#L2371):

- `CONFIG_NET_PKT_RX_COUNT=24`
- `CONFIG_NET_PKT_TX_COUNT=24`
- `CONFIG_NET_BUF_RX_COUNT=64`
- `CONFIG_NET_BUF_TX_COUNT=64`
- `CONFIG_NET_BUF_DATA_SIZE=2048`

From [`build_fan_controller/zephyr/.config`](D:/Projects/zephyr-ot-clean/build_fan_controller/zephyr/.config#L766):

- `CONFIG_ESP32_TIMER_TASK_STACK_SIZE=4096`

Interpretation:

- WiFi is configured to prefer PSRAM-backed allocation.
- AP+STA coexistence increases WiFi object count and queue/task activity.
- Network buffer counts are not tiny; the system is not in a trivial-memory configuration.
- Internal kernel heap is still only 128 KB, so corruption or fallback allocations can become visible there.

## 6. Where the `"memory allocation failed"` Log Comes From

The exact log string is emitted by:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L54)
- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L71)

Specifically:

- `wifi_malloc()`
- `wifi_calloc()`

Both call the heap adapter functions defined in:

- [`esp_heap_adapter.h`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/port/include/esp_heap_adapter.h#L25)

With the current config:

- WiFi allocations prefer `shared_multi_heap_aligned_alloc(... SMH_REG_ATTR_EXTERNAL ...)`
- frees go to `shared_multi_heap_free()` unless the pointer is in DRAM, in which case they go to `k_free()`

This matters because:

- the first visible problem is likely PSRAM-side pressure or misuse
- the final visible crash is in Zephyr heap bookkeeping on the internal heap side
- both can coexist if the adapter mixes object types or frees memory through the wrong path

## 7. Primary Root-Cause Candidate: Adapter Queue Wrapper Is Structurally Wrong

This is the strongest finding in the codebase.

### 7.1 Wrong object type used for queue allocation

The adapter allocates a `struct k_queue`:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L248)

Then immediately initializes the same storage as a `struct k_msgq`:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L257)

Relevant structure definitions:

- [`kernel.h`](D:/Projects/zephyr-ot-clean/zephyr/include/zephyr/kernel.h#L2253) defines `struct k_queue`
- [`kernel.h`](D:/Projects/zephyr-ot-clean/zephyr/include/zephyr/kernel.h#L5070) defines `struct k_msgq`

Why this is dangerous:

- `k_queue` and `k_msgq` are different kernel object types.
- They do not have the same layout.
- `k_msgq_init()` writes fields that do not exist in `struct k_queue`.
- Even if sizes happened to be close in one build, the layout mismatch is still invalid.

Expected failure pattern:

- silent corruption of adjacent memory or object headers
- later heap corruption when those objects are freed or reused
- crash delayed by minutes, not necessarily immediate

This matches the observed crash timing very well.

### 7.2 Shared global queue buffer across queue instances

The adapter uses one global buffer pointer:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L48)

It is assigned in `wifi_create_queue()`:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L94)

It is then reused in `queue_create_wrapper()`:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L248)

Why this is dangerous:

- if more than one queue is created, later calls overwrite the global `wifi_msgq_buffer`
- different queue objects can end up sharing one backing buffer
- queue operations can overwrite each other
- queue deletion does not clearly own and free the right backing store

This is a second independent corruption candidate.

### 7.3 Queue deletion leaks or loses the backing buffer

`wifi_delete_queue()` frees only:

- `queue->handle`
- `queue`

But not the global `wifi_msgq_buffer`:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L125)

Why this matters:

- repeated queue creation/deletion can leak memory
- leak pressure can explain the earlier `"memory allocation failed"` warnings
- leak plus layout misuse is a strong combined explanation for the delayed crash

## 8. Secondary Root-Cause Candidate: Adapter Task Wrapper Reuses One Global Thread Object and One Stack

The adapter defines:

- one stack: [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L46)
- one thread object: [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L50)

Both task-creation wrappers reuse those same globals:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L322)
- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L334)

Why this is dangerous:

- if the WiFi stack creates more than one task, object metadata is overwritten
- multiple tasks can share the same stack definition
- later abort/delete paths can operate on stale or aliased thread objects

This is not yet proven to be the active trigger in this run, but it is another serious defect in the same adapter.

## 9. Why the Crash Shows Up in `esp_timer`

The active crashing thread is `esp_timer`, but that does not mean `esp_timer` is the origin.

Relevant code:

- timer objects are allocated with `k_calloc()` in [`esp_timer.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/components/esp_timer/src/esp_timer.c#L121)
- timer objects are deleted/freed in [`esp_timer.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/components/esp_timer/src/esp_timer.c#L291)
- task-context delete/free happens inside [`esp_timer.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/components/esp_timer/src/esp_timer.c#L403)

Why this fits the observed fatal exception:

- if the heap was already corrupted by the adapter, `k_free(it)` or nearby heap operations in `esp_timer` can crash
- the timer thread is a frequent allocator/freeing context in a networked system
- heap corruption often surfaces in the next unrelated allocator activity, not at the original write

## 10. Application-Layer Code Review Result

The application code reviewed during this analysis does not currently present a comparably strong root-cause candidate.

Reviewed areas:

- [`wifi_manager.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/services/network/wifi_manager.cpp)
- [`main.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/main.cpp)
- [`memory_domains.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/core/memory_domains.cpp)

What was found:

- `wifi_manager.cpp` schedules reconnect and NTP work, but nothing there matches the heap corruption signature directly.
- `main.cpp` starts the expected services and worker threads, but there is no obvious repeating allocation pattern matching the crash.
- `memory_domains.cpp` places HTTP scratch buffers in PSRAM statically, which reduces pressure rather than causing dynamic corruption.

There may still be application pressure contributing to runtime memory exhaustion, but the adapter defects are materially stronger candidates than any application bug found so far.

## 11. Most Likely Failure Chain

Current best-fit sequence:

1. WiFi/AP+STA runs for several minutes under normal traffic.
2. The adapter creates or uses queue/task objects through invalid wrappers.
3. Heap-adjacent memory or queue backing storage is corrupted, or memory is leaked repeatedly.
4. Adapter begins to log `"memory allocation failed"`.
5. Later, `esp_timer` performs a `k_free()` or related heap operation.
6. Zephyr heap free-list metadata is already invalid.
7. Crash occurs in `chunk_set()` / `free_list_remove_bidx()`.
8. Zephyr fatal handler runs, then [`fatal_reboot.cpp`](D:/Projects/zephyr-ot-clean/app/fan_controller/src/core/fatal_reboot.cpp#L8) reboots.

## 12. Confidence Assessment

High confidence:

- the fatal PC is in Zephyr heap metadata maintenance
- the visible `"memory allocation failed"` log originates in the ESP32 WiFi adapter
- the adapter contains at least one objectively invalid queue implementation

Medium confidence:

- the queue wrapper defect is the direct corruption source in this exact run

Lower confidence:

- the timer thread itself owns the original bug
- application logic is the primary trigger

## 13. Recommended Next Investigation Steps

Priority order for the next engineer:

1. Fix or temporarily bypass the invalid WiFi adapter queue/task wrappers first.
2. Reproduce with heap validation and assertions enabled to catch the first bad write earlier.
3. Add periodic telemetry for:
   - internal heap free bytes
   - PSRAM free bytes
   - WiFi queue/object creation count
4. Re-run with AP+STA traffic and note whether allocation failures disappear after adapter fixes.
5. Only after adapter cleanup, investigate residual application-side leaks if the issue remains.

Useful debug toggles to consider in the next pass:

- `CONFIG_ASSERT=y`
- heap validation options for Zephyr heap debugging
- optional extra WiFi debug logging if needed

## 14. Open Questions for the Next Engineer

These were not fully resolved in this analysis pass:

1. How many times does the ESP WiFi stack call `_queue_create`, `_wifi_create_queue`, and task creation wrappers during AP+STA operation?
2. Is there a second corruption path via unimplemented wrappers such as:
   - [`queue_send_to_back_wrapper`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L297)
   - [`queue_send_to_front_wrapper`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L302)
   - [`event_group_wait_bits_wrapper`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c#L317)
3. Are the observed allocation failures entirely PSRAM-side, or is there also internal heap fallback/exhaustion before the fatal?

## 15. Handoff Summary

If another engineer picks this up cold, they should start from the adapter, not the application.

Start here:

- [`esp_wifi_adapter.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c)
- [`esp_heap_adapter.h`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/zephyr/port/include/esp_heap_adapter.h)
- [`esp_timer.c`](D:/Projects/zephyr-ot-clean/modules/hal/espressif/components/esp_timer/src/esp_timer.c)
- [`heap.c`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.c)
- [`heap.h`](D:/Projects/zephyr-ot-clean/zephyr/lib/heap/heap.h)

Bottom line:

- The visible reboot handler is not the bug.
- The fatal exception is a downstream symptom of heap corruption.
- The ESP32 Zephyr WiFi adapter contains multiple high-risk defects that are sufficient to explain the observed long-run crash.
