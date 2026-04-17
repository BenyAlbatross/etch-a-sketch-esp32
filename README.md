| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# etch-a-sketch ESP32 Firmware

The ESP32-S2 is the network bridge of the etch-a-sketch system. It owns the
canonical drawing state and connects three things together:

1. The **MCXC444** microcontroller (encoders, button, MPU-6050, RGB LED,
   buzzer) that produces pen-state packets over UART.
2. A **browser viewer** that wants near-real-time strokes over WebSocket.
3. A **backend HTTP API** that issues drawing prompts and runs the AI guess
   pipeline when the user submits.

## What this firmware does

- **UART ingress.** A dedicated FreeRTOS task tight-loops on `UART_NUM_1`,
  reads newline-terminated `$S,x,y,penDown,erase,submit\n` packets from the
  MCXC, and pushes parsed lines onto a static FreeRTOS queue.
- **Live viewer push (fast path).** A second task drains UART packets,
  parses them, and immediately publishes compact JSON stroke events to all
  connected WebSocket clients on `/ws` (subprotocol `etchsketch.v1.json`) so
  the browser stays responsive.
- **Framebuffer (authoritative state, slow path).** A third task receives the
  parsed states from a dedicated queue, validates each packet,
  and applies it to a 128x128 1-bit-per-pixel framebuffer
  (`components/etch_sketch_core/image_framebuffer.c`). Strokes between consecutive pen-down samples
  are filled with Bresenham line rasterization so the rendered drawing stays
  continuous even when the encoder skips pixels.
- **Submit pipeline.** When the MCXC sets `submit=1`, the firmware base64-
  encodes the framebuffer into a `{"type":"frame",...}` JSON envelope and
  POSTs it to the backend's drawing-submit endpoint, then fetches the next
  prompt and publishes it back over the WebSocket.

  The submit request instruction sent to the model is:

  > "Grade this Etch-a-Sketch drawing against the target word 'triangle'. Use
  > confidence 1 for no match and 10 for excellent match; set correct true at
  > confidence 6 or higher."

  The drawing is only graded correct if the AI's returned confidence passes
  the threshold (`APP_SUBMIT_CONFIDENCE_PASS_THRESHOLD` in `app_api.c`, default
  6). `'triangle'` is substituted with the active prompt word.
- **Wi-Fi + HTTP client.** Joins the Wi-Fi network defined in `secrets.h`,
  performs the prompt-fetch and submit calls with `esp_http_client`, and
  hosts the local WebSocket server with `esp_http_server`.
- **Diagnostics.** Periodic logs of UART pipeline counters
  (`lines / parsed / malformed / queueDrops / queueDepth`), WebSocket client
  count, and per-task stack high-water marks make bring-up easier.

## RTOS task priorities

Current application task priorities are set in `main/main.c`:

- `state_dispatch_task` = 6
- `uart_rx_task` = 5
- `framebuffer_task` = 4
- `viewer_task` = 3
- `api_task` = 3

Rationale for the ordering:

- Queue stage 1 (`s_uart_packet_queue`): `state_dispatch_task` is the consumer
  and `uart_rx_task` is the producer, so consumer priority is higher to drain
  bursts quickly.
- Framebuffer stage (`s_framebuffer_state_queue`): `framebuffer_task` runs below
  `uart_rx_task` and `state_dispatch_task`, but above viewer/API work, so the
  authoritative drawing state continues to drain after the live path accepts a
  parsed sample.
- `viewer_task` and `api_task` share the lowest application priority in this
  tuning profile. They are both fed by bounded queues, so use UART pipeline
  counters (`fbDrops`, `fbDepth`, `viewerDrops`, `viewerDepth`) to verify this
  balance remains healthy under sustained input.

Reference:

- ESP-IDF FreeRTOS task scheduling and priorities:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/system/freertos_idf.html

## Packet format mapping (UART vs WebSocket)

The browser does **not** consume raw UART ASCII. UART is parsed on the ESP32,
then forwarded as JSON over WebSocket.

UART input accepted by ESP32 parser:

- `$S,x,y,pen`
- `$S,x,y,pen,erase`
- `$S,x,y,pen,erase,submit`

Viewer event emitted by ESP32:

- `{"type":"stroke","x":<0-127>,"y":<0-127>,"penDown":<0|1>,"erase":<0|1>}`

Field mapping:

- `x` -> `x`
- `y` -> `y`
- `pen` -> `penDown`
- `erase` -> `erase`
- `submit` -> internal framebuffer/AI-submit control only (not forwarded in
  `stroke` event)

Why `type` is needed:

- The browser multiplexes different message kinds on one WebSocket (`stroke`,
  `clear`, `result`, `prompt`) and routes by `data.type`.
- Without `type`, the browser would need heuristic field inference or
  separate WebSocket channels.

## Hardware wiring

UART link to the MCXC444 is on `UART_NUM_1` at **115200 baud, 8-N-1**.

| Wire colour | From                | To                   |
| ----------- | ------------------- | -------------------- |
| Yellow      | MCXC PTE22 (TX)     | ESP32 GPIO16 (RX)    |
| Grey        | ESP32 GPIO17 (TX)   | MCXC PTE23 (RX)      |
| —           | MCXC GND            | ESP32 GND            |

A common ground between the two boards is required.

> **Do not move the UART RX off GPIO16.** GPIO18 on the ESP32-S2 DevKit is
> wired to the onboard RGB LED net. Putting any external load on GPIO18
> backfeeds that net and is enough to break the auto-reset / UART sync the
> first time you try to flash. Keep external wiring off GPIO18.

## Configuration

Wi-Fi credentials and the backend API base URL / token live in
`main/secrets.h` (not committed). Build pin assignments and timing live at the
top of `main/main.c`:

- `UART_PORT_NUM` — `UART_NUM_1`
- `UART_BAUD_RATE` — `115200`
- `UART_TX_PIN` — `17`
- `UART_RX_PIN` — `16`

The MCXC side must agree on baud rate and pinout — see
`etch-a-sketch-mcxc/source/MCXC444_Project.c` (`BAUD_RATE`, `UART_TX_PIN`,
`UART_RX_PIN` on `PORTE`).

### Local debug toggle (submit path)

There are two related debug knobs in the ESP32 firmware:

1. **Enable/disable local debug submit mode**
  - File: `components/etch_sketch_core/app_api.c`
  - Macro: `APP_API_LOCAL_DEBUG_MODE`
  - Set to `1`: submit returns a local simulated result (no OpenAI call).
  - Set to `0`: submit uses the OpenAI path (when API key/config are valid).

2. **Choose simulated result correctness (when local debug mode is ON)**
  - File: `main/main.c`
  - Variable: `s_local_debug_submit_correct`
  - Set to `true`: submit emits a correct result (`$C,RESULT,1`).
  - Set to `false`: submit emits a wrong result (`$C,RESULT,0`).

Quick toggle workflow:

```c
/* components/etch_sketch_core/app_api.c */
#define APP_API_LOCAL_DEBUG_MODE 1

/* main/main.c */
static bool s_local_debug_submit_correct = true;
```

After changing either value, rebuild and flash the ESP32 firmware.

### TLS certificate bundle requirement (API)

For HTTPS API calls to work reliably (prompt fetch + drawing submit), keep
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` enabled.

If this option is disabled, TLS verification can fail for API certificate
chains that depend on cross-signed roots, and ESP32 API calls will fail before
application-level JSON handling.

To check or enable it:

```sh
idf.py menuconfig
```

Then enable `MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` under mbedTLS
certificate-bundle options and rebuild.

## Startup and recovery

Recommended startup order:

1. Start or flash the ESP32 first.
2. Wait for Wi-Fi, WebSocket, UART, and RTOS task startup logs.
3. Start or reset the MCXC.

The ESP32 owns the prompt, browser phase, WebSocket snapshots, and submit
result state. The MCXC owns the joystick, pen state, LEDs, buzzer, and UART
control flags. On boot, MCXC starts in `ROUND_PHASE_WAITING_PROMPT` with
`promptRequest=1`, so it immediately asks ESP32 for a prompt.

Expected startup states:

1. **ESP32 first, then MCXC.** This is the preferred path. ESP32 is ready before
   MCXC starts sending `$S,...,promptRequest=1` packets. ESP32 should publish a
   prompt to the browser, send `$C,PROMPT,1`, and MCXC should unlock to blue.
2. **MCXC first, while still waiting for prompt, then ESP32.** This should
   recover because MCXC keeps sending `promptRequest=1` until it receives
   `$C,PROMPT,1`. Once ESP32 starts, it should generate or reuse a prompt, send
   prompt-ready, and MCXC should unlock to blue.
3. **MCXC already blue, then ESP32 restarts.** This is a recovery case, not a
   clean continuation. ESP32 has lost its RAM prompt state, so it should treat
   startup as a new prompt authority event, publish a fresh prompt to the
   browser, and send `$C,PROMPT,1` again. MCXC should remain unlocked, but may
   recenter because its `PROMPT=1` handler recenters the cursor.

Duplicate startup prompt requests are acceptable if they are ignored after ESP32
has already reached drawing phase. A log such as
`Ignoring prompt response id=<n> while phase=1` means ESP32 was already in
`APP_PHASE_DRAWING` and rejected a late prompt response instead of changing the
prompt mid-round.

## Building and flashing

This is a standard ESP-IDF project. **Build it from this directory only** —
the parent workspace contains other components (the MCXC project, the web
client) that are not part of this build tree.

```sh
cd etch-a-sketch-esp32
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

If `esptool` fails during the initial connect with a `get_security_info`
traceback, the most common causes on this board are:

1. Something is wired to **GPIO18** — disconnect it and retry.
2. A serial monitor is still holding the port — close it and retry.
3. The board didn't enter download mode — hold `BOOT`, tap `RST`, release
   `BOOT`, then re-run `flash`.

## Host utility check (`tools/`)

`tools/write_circle_png.c` is a standalone host utility for quickly validating
the 128x128 1-bit PNG output path without flashing the ESP32. It draws a ring
into a test framebuffer and writes a PNG file.

Compile and run from this project root:

```sh
cd etch-a-sketch-esp32
cc -std=c11 -Wall -Wextra -Werror -O2 tools/write_circle_png.c -o build/write_circle_png
./build/write_circle_png build/circle.png
```

Quick checks:

```sh
file build/circle.png
# expected: PNG image data, 128 x 128, 1-bit grayscale, non-interlaced

ls -lh build/circle.png

open build/circle.png
# expected: a black ring on a white background
```

## VS Code / ESP-IDF workspace files

The `.vscode/` folder is VS Code workspace configuration, not ESP-IDF firmware
source. The ESP-IDF VS Code extension can generate it, but it commonly contains
developer-specific values such as local serial ports, toolchain paths,
`clangd.path`, and absolute build paths.

Recommended team practice:

- Do not treat `.vscode/` as portable project source unless the files have been
  sanitized for the whole team.
- Prefer ignoring `.vscode/` in Git for this project. If the folder is already
  tracked, adding it to `.gitignore` is not enough; remove it from the index
  with `git rm --cached -r .vscode` after the team agrees.
- Regenerate the folder in VS Code from the Command Palette with
  `ESP-IDF: Add VS Code Configuration Folder`.
- Espressif documents this command here:
  https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/commands.html
- Espressif documents VS Code workspace-folder settings here:
  https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/settings.html

## Unit tests

Unity tests live with the testable component, and the `test/` directory is a
separate ESP-IDF unit-test app.

```sh
cd etch-a-sketch-esp32/test
idf.py -DIDF_TARGET=esp32s2 build
idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

When the Unity app is idle, press Enter in the serial monitor to print the test
menu. Type `*` to run all tests, `[image_framebuffer]` to run framebuffer tests,
or `[app_api]` to run API helper tests.

`pytest_unittest.py` is optional automation around the same Unity test app:

```sh
cd etch-a-sketch-esp32/test
pytest --target esp32s2 --build-dir /path/to/test/build
```

## Project layout

```
etch-a-sketch-esp32/
├── CMakeLists.txt
├── sdkconfig                  ESP-IDF build config (target = esp32s2)
├── components/
│   └── etch_sketch_core/
│       ├── CMakeLists.txt
│       ├── app_api.c
│       ├── app_api.h
│       ├── image_framebuffer.c
│       ├── image_framebuffer.h
│       ├── idf_component.yml
│       └── test/
│           ├── CMakeLists.txt
│           └── test_image_framebuffer.c
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 Wi-Fi, UART tasks, HTTP server, WS, app orchestration
│   └── secrets.h              Wi-Fi creds + OpenAI API key (gitignored)
├── test/                      ESP-IDF Unity test-app project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── pytest_unittest.py
│   └── main/
│       ├── CMakeLists.txt
│       └── test_app_main.c
├── WIKI-ESP32.md              Iteration notes / design log
└── README.md                  (this file)
```

## Related components

- `etch-a-sketch-mcxc/` — MCXC444 firmware that drives the encoders, button,
  IMU, buzzer, and produces the UART packet stream consumed here.
- `etch-a-sketch-web/` — browser viewer that connects to the ESP32's
  `/ws` endpoint.
