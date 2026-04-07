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

## Backlog TODO

- [x] Add runtime stack watermark logs for both tasks to tune stack sizes safely.
- [x] Convert task/queue names and constants into a small RTOS config block to make future review easier.

