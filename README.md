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
  (`main/image_framebuffer.c`). Strokes between consecutive pen-down samples
  are filled with Bresenham line rasterization so the rendered drawing stays
  continuous even when the encoder skips pixels.
- **Submit pipeline.** When the MCXC sets `submit=1`, the firmware base64-
  encodes the framebuffer into a `{"type":"frame",...}` JSON envelope and
  POSTs it to the backend's drawing-submit endpoint, then fetches the next
  prompt and publishes it back over the WebSocket.
- **Wi-Fi + HTTP client.** Joins the Wi-Fi network defined in `secrets.h`,
  performs the prompt-fetch and submit calls with `esp_http_client`, and
  hosts the local WebSocket server with `esp_http_server`.
- **Diagnostics.** Periodic logs of UART pipeline counters
  (`lines / parsed / malformed / queueDrops / queueDepth`), WebSocket client
  count, and per-task stack high-water marks make bring-up easier.

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

## Project layout

```
etch-a-sketch-esp32/
├── CMakeLists.txt
├── sdkconfig                  ESP-IDF build config (target = esp32s2)
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 Wi-Fi, UART task, HTTP server, HTTP client, WS
│   ├── image_framebuffer.c    1bpp framebuffer, packet parser, line raster
│   ├── image_framebuffer.h
│   └── secrets.h              Wi-Fi creds + API base/token (gitignored)
├── WIKI.md                    Iteration notes / design log
└── README.md                  (this file)
```

## Related components

- `etch-a-sketch-mcxc/` — MCXC444 firmware that drives the encoders, button,
  IMU, buzzer, and produces the UART packet stream consumed here.
- `etch-a-sketch-web/` — browser viewer that connects to the ESP32's
  `/ws` endpoint.
