# ESP32 Image Encoding Notes

## 2026-04-05 - Iteration 1 (Input-to-Frame Encoding)

Implemented a first-pass image framebuffer module on ESP32 that converts UART input packets into a persistent 900x600 framebuffer.

### What was added

- `main/image_framebuffer.h` and `main/image_framebuffer.c`
- Integration sample in `main/main.c`

### Software design decisions

- **Canvas representation**: bit-packed monochrome buffer (1 bit per pixel).
	- Memory cost: 900 * 600 = 540,000 bits = 67,500 bytes.
	- Chosen for deterministic memory usage on ESP32 and straightforward serialization.

- **Input contract**: parser for unchanged UART packet format:
	- `$S,x,y,penDown,erase,submit\n`
	- Validates coordinate bounds and boolean flags.

- **Stroke model**: Bresenham line rasterization between consecutive pen-down points.
	- Ensures continuous lines when encoder updates skip pixels.

- **Socket payload format (current)**:
	- JSON envelope with base64-encoded bit-packed framebuffer:
	- `{"type":"frame","width":900,"height":600,"format":"1bpp-msb","data":"..."}`
	- This is a transport-safe baseline for websocket push integration.

### Why not PNG yet

- PNG encoding on embedded targets introduces additional complexity and dependencies.
- This iteration isolates core concerns (state parsing, drawing correctness, deterministic memory model) before introducing compression and API-specific image formats.
- Next iteration can layer PNG conversion from the same framebuffer without changing input handling logic.

### Known next step

- Add PNG encoding path from framebuffer for submission payloads to vision API.

## 2026-04-05 - Iteration 2 (Live UART Pipeline)

Replaced demo/sample packet feeding with a live UART1 read loop in app runtime.

### What was added

- UART1 initialization in `main/main.c` at 9600 baud.
- Line-oriented UART packet reader with overflow protection.
- Single packet processing function:
	- parse packet
	- apply to framebuffer
	- on `submit=1`, build socket payload and call socket send hook

### Software design decisions

- **Single-responsibility runtime functions**:
	- `app_uart_init()` for peripheral setup
	- `app_uart_read_packet_line()` for line framing
	- `app_process_packet_line()` for domain logic
	- `app_socket_send_frame()` as integration seam for teammates' socket layer

- **Memory safety adjustment**:
	- Socket payload buffer remains heap-allocated once at startup and reused in loop.
	- Avoids large static `.bss` allocations that can overflow DRAM on ESP32-S2.

- **Packet robustness**:
	- Oversized UART lines are dropped and drained until newline to preserve stream synchronization.

- **UART assignment**:
	- Firmware now pins communication to `UART_NUM_1` for the external MCU link.
	- `UART0` remains available for USB serial flashing and monitor logs.

- **Debugging note (pin sharing)**:
	- Current UART setup uses `U1RXD` on GPIO18.
	- On ESP32-S2-DevKitM-1, GPIO18 is also connected to the onboard RGB LED data line.
	- If UART behavior looks unstable, or LED behavior looks unexpected, check for this shared-pin interaction first.

### Known next step

- Replace `app_socket_send_frame()` hook with actual websocket send implementation.

## 2026-04-05 - Iteration 3 (RTOS Task Split)

Refactored the runtime loop into two FreeRTOS tasks with a queue boundary.

### What was added

- `uart_rx_task`:
	- reads UART lines
	- pushes packet strings into a queue

- `framebuffer_task`:
	- receives packet strings from queue
	- applies packet to framebuffer
	- on submit, builds socket payload and calls socket send hook

- Queue between tasks:
	- bounded queue depth to decouple UART ingress from payload generation latency
	- queue now instantiated with `xQueueCreateStatic()`

- Static RTOS object allocation:
	- `uart_rx_task` now created with `xTaskCreateStatic()`
	- `framebuffer_task` now created with `xTaskCreateStatic()`

### Software design decisions

- **Producer-consumer pipeline**:
	- UART ingress is isolated from image processing and socket push path.
	- This reduces timing coupling and simplifies future profiling/tuning.

- **Backpressure behavior**:
	- If queue is full, newest UART packet is dropped with a warning log.
	- This avoids blocking UART ingestion indefinitely.

- **Single writer for framebuffer state**:
	- only `framebuffer_task` mutates framebuffer state, minimizing synchronization complexity.

- **Submit semantics split**:
	- Viewer updates are now emitted continuously per valid input packet (near real-time stroke stream intent).
	- `submit=1` is reserved for the AI guess pipeline trigger (encode framebuffer and send to AI API integration hook).
	- This avoids coupling on-screen refresh behavior to the submit action.

- **Task affinity policy (why we are not pinning to a core)**:
	- Target is ESP32-S2 (single-core mode), so core-affinity behavior is effectively not meaningful for our app tasks.
	- We keep `xTaskCreate()` (unpinned semantics) for portability and cleaner code paths across targets.
	- If this code is later moved to a dual-core target, we can revisit pinning based on measured contention and timing.

- **Static allocation candidates (next hardening step)**:
	- Convert `uart_rx_task` to static allocation (`xTaskCreateStatic`) because it is long-lived and always present.
	- Convert `framebuffer_task` to static allocation (`xTaskCreateStatic`) for the same reason.
	- Convert the packet queue to static allocation (`xQueueCreateStatic`) to eliminate queue heap usage and improve startup determinism.
	- Keep transient/size-variable buffers (such as large payload buffers) dynamic unless we define strict maximum sizing and memory budget.

### RTOS references used

- FreeRTOS tasks in ESP-IDF (`xTaskCreate`, priorities, stack sizing):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

- FreeRTOS queues in ESP-IDF (`xQueueCreate`, `xQueueSend`, `xQueueReceive`):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

- Single-core mode behavior and why affinity is a no-op on ESP32-S2:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html#single-core-mode

- Supplemental task-affinity APIs (`...PinnedToCore`) for when moving to multicore targets:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_additions.html

## 2026-04-05 - Iteration 4 (WebSocket Send Integration)

Implemented ESP-IDF WebSocket server send path and replaced the previous socket-send placeholder.

### What was added

- `main/main.c` now starts an HTTP server with WebSocket endpoint:
	- URI: `/ws`
	- subprotocol: `etchsketch.v1.json`

- `app_socket_send_frame()` now:
	- retrieves current client list from HTTP server
	- filters active WebSocket clients
	- sends payload as text frame using `httpd_ws_send_frame_async()`

- `main/CMakeLists.txt` now includes:
	- `esp_http_server` in `PRIV_REQUIRES`

### Software design decisions

- **Graceful startup/fallback**:
	- If WebSocket support/config is unavailable at runtime, firmware continues and logs warning.
	- This keeps UART/framebuffer pipeline alive even when network stack is not ready.

- **Near-real-time viewer path unchanged**:
	- Viewer stroke payloads still emit per valid input packet.
	- Submit remains reserved for AI submit trigger path.

- **No-client behavior**:
	- Sending with zero connected WebSocket clients is treated as a non-error to avoid noisy fault logs.

- **WebSocket diagnostics for integration**:
	- Added throttled runtime logs for connected WebSocket client count.
	- Logs are emitted on handshake, periodic intervals, and partial-send conditions.
	- Intended to quickly distinguish connection issues from drawing-pipeline issues during bring-up.

### RTOS and API references used

- ESP-IDF HTTP Server + WebSocket API:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/protocols/esp_http_server.html

- FreeRTOS in ESP-IDF (task/queue context for sender task):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - Build System Note (Project Rename)

After renaming the project folder and/or CMake `project(...)` name, ESP-IDF may fail with Ninja regenerate errors because the generated build metadata still contains absolute paths from the old location.

### Symptom

- Build fails around CMake regenerate, for example:
	- `ninja: error: rebuilding 'build.ninja': subcommand failed`
	- old source path appears in generated `build/*` CMake files.

### Recovery

- Run ESP-IDF full clean to remove stale generated metadata.
- Rebuild from the current workspace path.

Validated in this repo on 2026-04-07:

- Fresh build regenerated artifacts using current path:
	- `/Users/benjamin/Documents/CodingProjects/etch-a-sketch-main/etch-a-sketch-esp32`
- App and bootloader binaries generated successfully.

## 2026-04-07 - esp_timer Stack Overflow Mitigation

Observed panic:

- `***ERROR*** A stack overflow in task esp_timer has been detected`
- Backtrace included Wi-Fi internal timer processing (`ieee80211_timer_process`).

### What changed

- Raised `CONFIG_ESP_TIMER_TASK_STACK_SIZE` from `3584` to `6144` in `sdkconfig`.

### Software engineering rationale

- `esp_timer` is a shared system timer task used by multiple subsystems (including Wi-Fi internals), not only app-level timer callbacks.
- Overflow in this task indicates configuration pressure on a shared runtime service stack, so increasing the stack is a minimal-risk, bounded mitigation.
- This keeps app architecture unchanged while removing the immediate crash condition.

### Verification plan

- Rebuild and flash.
- Monitor boot/runtime logs and confirm no repeated `esp_timer` stack overflow panic under normal drawing + Wi-Fi load.

### References (ESP-IDF / FreeRTOS docs)

- ESP Timer API (task-based dispatch model and timer behavior):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/esp_timer.html

- ESP-IDF FreeRTOS overview (task stack/high-watermark concepts used for diagnosis):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - UART Bring-Up Diagnostics

Added a low-noise ESP32 log for parsed MCXC input state changes:

- `UART input: x=<n> y=<n> penDown=<0|1> erase=<0|1> submit=<0|1>`

### Software engineering rationale

- The MCXC sends `$S,x,y,penDown,erase,submit` continuously, so the ESP32 should log an initial state even before any encoder or button input.
- Logging only on parsed state changes keeps the monitor readable while still distinguishing UART/parser faults from WebSocket/browser faults.
- This did not add a new RTOS primitive; it is ordinary task-context diagnostics in the existing UART-to-framebuffer consumer path.

## 2026-04-07 - Live Drawing Latency Tuning

Raised the MCXC-to-ESP32 UART link from 9600 baud to 115200 baud and reduced the MCXC state publish period from 50 ms to 5 ms.

### Software engineering rationale

- At 9600 baud, each ASCII state packet consumes a meaningful fraction of the 50 ms send period.
- At 115200 baud, the same packet is short enough that a 5 ms update cadence is practical for real-time drawing.
- The WebSocket path already emits one stroke JSON per parsed UART packet, so increasing the UART producer cadence directly improves browser update cadence without changing the socket protocol.
- The ESP32 and MCXC baud rates must match; changing only one side will break the UART link.

## 2026-04-07 - UART Queue Saturation Mitigation

Observed runtime warning under sustained drawing input:

- `UART packet queue full, dropping packet`

### What changed

- Increased UART driver RX ring buffer from `512` to `2048` bytes.
- Increased UART packet queue depth from `16` to `64` messages.
- Changed task priorities so queue consumer preempts producer when backlog appears:
	- `socket_dispatch_task`: `4 -> 6`
	- `uart_rx_task`: remains `5`
	- `framebuffer_task`: `3 -> 4`
- Changed UART queue send timeout from `20 ms` to `0 ms` (non-blocking enqueue attempt) to avoid producer-side blocking stalls that can worsen UART RX overrun risk.
- Throttled verbose `UART input` state-change logs to a max of 10 Hz to reduce avoidable CPU/log I/O overhead in the consumer path.

### Software engineering rationale

- The queue-full symptom indicates sustained producer throughput exceeding consumer throughput.
- With producer priority above consumer, backlogs can self-amplify under bursty/high-rate input.
- Prioritizing the consumer and adding bounded buffering is a standard producer-consumer stabilization pattern in RTOS systems.
- Reducing log pressure in hot paths removes non-functional work that can hide as latency under real-time input.

### References (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS task scheduling and priorities:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

- ESP-IDF FreeRTOS queues (`xQueueSend`, `xQueueReceive`, timeouts):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## Backlog TODO

- [x] Add runtime stack watermark logs for both tasks to tune stack sizes safely.
- [x] Convert task/queue names and constants into a small RTOS config block to make future review easier.

## 2026-04-07 - Submission Stub + MCU RESULT Protocol (Increment 1)

Started methodical implementation for submit/receive decoupling from teammate-owned API integration.

### What changed

- Introduced a temporary submit stub path in `main/main.c`:
	- `app_submit_stub_request_result(...)`
	- Controlled by a single flag: `s_submit_stub_success_flag` (true/false)
- Changed submit pipeline wiring so `app_submit_drawing_for_ai(...)` now:
	- requests a stubbed submit result
	- always emits browser result JSON (`guess`, `confidence`, `correct`)
	- refreshes next prompt only when submit success is true
- Updated MCXC command behavior for submit outcomes:
	- replaced confidence-derived `RATE` + `DONE` sends
	- now sends only `$C,RESULT,<0|1>\n` using a dedicated helper

### Software engineering rationale

- This isolates integration risk by keeping submit trigger and transport flow intact while replacing only the API dependency edge.
- A single boolean stub flag enables deterministic success/failure path testing without network dependence.
- Browser schema stability is preserved, so frontend work can continue unchanged while backend integration is pending.
- MCXC now receives only success/failure, matching updated actuator ownership (green/red LED decision on MCXC side).

### Team boundary / handoff

- Teammate-owned work should replace the body of `app_api_submit_drawing(...)` while preserving the function contract expected by the submit pipeline.
- Do not re-introduce `RATE`/`DONE` in this path unless protocol ownership changes again.

### RTOS note

- No new RTOS primitives were introduced in this increment.
- Existing task/queue topology remains unchanged (`uart_rx_task` -> `socket_dispatch_task` -> `framebuffer_task`).

### References (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS task and queue behavior:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - Submit Rising-Edge Latch (Increment 2)

Added submit edge-detection to prevent repeated submissions when the MCU keeps `submit=1` across multiple UART state packets.

### What changed

- In `main/main.c`, `app_process_framebuffer_state(...)` now uses a local static submit-level latch:
	- submission triggers only on `0 -> 1` transition
	- while `submit` stays high, additional packets do not retrigger API submit
	- latch rearms when `submit` returns to `0`

### Software engineering rationale

- The UART producer can emit state at high cadence, so level-triggered submit causes duplicate submissions for a single button hold.
- Rising-edge gating converts submit into a one-shot event per press without changing packet schema.
- The latch is scoped to the framebuffer consumer path, which is already single-writer/single-consumer for authoritative state.

### RTOS note

- No new RTOS primitive was introduced.
- The change is a task-local state machine within existing queue consumer execution.

### Reference (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS behavior (tasks/queues in ESP-IDF):
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - Explicit Submit Event Channel (Increment 3)

Refactored framebuffer queue payload to carry explicit event type so submit is represented as a command event rather than being handled inside state-apply logic.

### What changed

- `main/main.c` queue message now includes event type:
	- `FRAMEBUFFER_EVENT_APPLY_STATE`
	- `FRAMEBUFFER_EVENT_SUBMIT`
- Fast path now creates events in order:
	- enqueue apply-state event for every valid packet
	- enqueue submit event only on submit rising edge (`0 -> 1`)
- Framebuffer task now dispatches by event type:
	- apply-state event -> mutate framebuffer
	- submit event -> trigger AI submit pipeline

### Software engineering rationale

- Separates data-plane updates (strokes) from command-plane intent (submit).
- Preserves deterministic ordering by using one queue and enqueue order.
- Improves extensibility for future command events without changing state payload semantics.

### RTOS note

- Still uses existing producer/consumer tasks and queue primitives.
- No additional tasks, mutexes, or synchronization primitives were introduced.

### Reference (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS tasks and queues:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - API Module Extraction + Stub Latency (Increment 4)

Refactored API-related logic into dedicated files so teammate-owned API work can be edited without touching queue/task logic in `main/main.c`.

### What changed

- Added new API module files:
	- `main/app_api.h`
	- `main/app_api.c`
- Moved prompt-fetch and prompt-publish API workflow into `app_api.c` via:
	- `app_api_fetch_and_publish_prompt(...)`
- Replaced in-file submit stub with module function:
	- `app_api_submit_drawing(...)`
- Added simulated API latency in submit stub:
	- `vTaskDelay(pdMS_TO_TICKS(2000))`
- Updated `main/main.c` to call module APIs and removed duplicated local HTTP/API helper functions.
- Updated `main/CMakeLists.txt` to compile `app_api.c`.

### Software engineering rationale

- Isolates teammate-owned API integration surface from RTOS dataflow and queue-event orchestration.
- Keeps compile-time contract explicit in `app_api.h`.
- Stub latency provides realistic timing for integration tests involving queue pressure and UI responsiveness.

### Team boundary / handoff

- Teammate should modify only `main/app_api.c` implementations for real API calls.
- `main/main.c` should remain focused on UART ingest, event routing, framebuffer mutation, and MCU/UI side effects.

### RTOS note

- Stub latency uses task delay (`vTaskDelay`) in the framebuffer consumer task context.
- This intentionally simulates blocking submit latency while preserving task scheduling semantics.

### References (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS task API (`vTaskDelay`) and scheduling behavior:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## 2026-04-07 - Prompt Request Handshake + Explicit Round Unlock (Increment 5)

Implemented prompt-request signaling from MCU to ESP32 and explicit prompt-ready acknowledgment from ESP32 back to MCU.

### What changed

- Extended parsed MCU state packet schema in `main/image_framebuffer.*`:
	- from: `$S,x,y,penDown,erase,submit`
	- to:   `$S,x,y,penDown,erase,submit,promptRequest`
	- parser remains backward compatible with 3/4/5-field variants.
- Added new framebuffer event type in `main/main.c`:
	- `FRAMEBUFFER_EVENT_PROMPT_REQUEST`
- Added prompt request rising-edge latch in UART fast path:
	- enqueues one prompt-request event per `0 -> 1` transition of `promptRequest`.
- Added MCU prompt-ready command from ESP32:
	- `$C,PROMPT,1\n`
	- emitted after successful prompt fetch/publish.
- Kept submit result command as:
	- `$C,RESULT,<0|1>\n`
- Removed automatic prompt refresh after submit:
	- submit now only emits result
	- next prompt is fetched only on explicit `promptRequest` event.

### Software engineering rationale

- Separates command intent into two explicit channels:
	- `submit` -> evaluate current drawing
	- `promptRequest` -> fetch next prompt and unlock next round
- Keeps queue/event architecture extensible while maintaining strict ordering.
- Rising-edge latches avoid repeated command execution when MCU holds a level high across multiple UART packets.

### Protocol effect

- MCU can now enforce strict lock/unlock semantics by waiting for `$C,PROMPT,1\n` before re-enabling drawing.
- ESP32 no longer advances prompts implicitly after submit; round advancement becomes explicit and deterministic.

### References (ESP-IDF / FreeRTOS docs)

- ESP-IDF FreeRTOS tasks/queues in ESP-IDF:
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html
