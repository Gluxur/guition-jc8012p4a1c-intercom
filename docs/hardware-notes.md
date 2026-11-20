# Hardware notes and measured pitfalls

Everything here was found the hard way on the JC8012P4A1C, most of it with a
measurement behind it. Several of these failures are **silent**: no error, no
log, just a degraded device. Read this before changing `sdkconfig`, the BSP or
the go2rtc stream.

## Silicon facts

- **CPU tops out at 360 MHz.** This board carries pre-v3 ESP32-P4 silicon; the
  400 MHz option is hidden by Kconfig.
- **No H.264 decoder.** The P4's H.264 hardware is an encoder only. Software
  decoding took 15-36 ms per frame and a full core; the project uses MJPEG and
  the hardware JPEG decoder (~5 ms per 800×1280 frame) instead.
- **No radio on the P4.** Wi-Fi goes through the ESP32-C6 over SDIO
  (`esp_wifi_remote` + `esp_hosted`). The C6 on this board runs esp_hosted
  2.x firmware; the "version mismatch" warning at boot (C6 2.3.0 vs host 2.12.x)
  is benign.
- **One SDMMC controller** shared by the microSD card and the SDIO link to the
  C6.

## Video

- **Hardware JPEG decoder**
  - Create the engine **at boot**: it needs internal DMA-capable RAM, gone
    once WebRTC is up.
  - Output is aligned to **16 px**: size the buffer on `(w+15)&~15` and pass
    that stride to the PPA, otherwise the picture is tiled.
  - Use `rgb_order = BGR` (= RGB565 little-endian as the PPA expects),
    otherwise you get rainbow colours.
- **Zero-copy path**: a source exactly `kLcdHRes` (800 px) wide, unscaled and
  16-aligned, is decoded directly into the DSI back buffer. That is why the
  go2rtc stream is transposed and padded to 800×1280. It removed a 22 ms/frame
  hidden memcpy through the PPA.
- **YUV420 on the P4 is packed by line** (`O_UYY_E_VYY`: odd lines `U Y Y…`,
  even lines `V Y Y…`) for the PPA, the JPEG codec, DMA2D CSC and the H.264
  encoder — not planar I420. Feeding I420 gives a 9-tile image.
- **PPA: never `PPA_TRANS_MODE_BLOCKING`.** Its wait is `portMAX_DELAY`; one
  lost transaction freezes the caller forever (dead screen, system alive, logs
  still flowing). Use `NON_BLOCKING` + a bounded wait.
- **PPA erratum DIG-734**: IDF computes the FIFO workaround *before* rotation.
  A badly sized band (measured: 224×68) stalls the **shared** SRM engine, i.e.
  every PPA client, for the rest of the session. `bsp/ppa_srm_quirk.cpp`
  splits bands to avoid it (host-tested).
- **Two PPA clients** (`ppa_video_`, `ppa_lvgl_`), never shared between tasks.
- PPA/DMA buffers: `heap_caps_aligned_alloc(64, …, MALLOC_CAP_SPIRAM)`. If
  corruption appears, suspect cache coherency (`esp_cache_msync()`).

## Display (MIPI-DSI)

- **`kLcdVfp = 120`, do not reduce.** The DSI driver restarts its DMA in
  software every frame, within the vertical blanking window. At `vfp=20` that
  window was 704 µs, missed every 5-20 s: the panel showed the same image for
  57 ms instead of 29 → permanent flicker on every screen, **with no
  `underrun` message**. Raising `dpi_clock_freq_mhz` without raising `vfp`
  shortens the window and brings the bug back (70 MHz: much worse).
- **LVGL stays in `RENDER_MODE_PARTIAL`.** FULL mode rotated 1 Mpx with the PPA
  (42.6 ms) on every flush, even to repaint a label.
- **Never flush through `esp_lcd_panel_draw_bitmap`.** It copies with the CPU
  and `esp_cache_msync`s full-width rows; after a 270° rotation one LVGL band
  spans the full 1280-line height → 2 MB of cache writeback per flush, ~14 MB
  per full repaint. The PPA writes the rotated band straight into the
  scanned-out framebuffer instead.
- **Two framebuffers = two contents to maintain.** In PARTIAL mode LVGL only
  writes to the buffer being scanned out. Any area the video does not rewrite
  (letterbox bars) must be cleared in **both** buffers
  (`dsi_frame_buffer_at()`), or the previous screen reappears behind the video
  on every flip.
- The displayed buffer is read from `shown_idx_`, never by inverting
  `write_idx_` (DPI starts on `fbs[0]`, hence `write_idx_{1}` at boot).
- LVGL draw buffers: **internal SRAM, 2 × 32 lines**. In PSRAM software
  rendering caps at 7.6 Mpx/s.

## Radio and clocks

- **Flash in DIO, never QIO.** Measured with flash mode as the only variable:
  QIO → RSSI −87..−92 dBm, 72 KB/s, 1-2 fps; DIO → −65..−68 dBm,
  1080-1224 KB/s, 15-17 fps. Four lines switching at 80 MHz millimetres from
  the C6 antenna desensitise its receiver. QIO "works": the boot log happily
  says `SPI Mode : QIO`.
  - **Signature** of any clock tweak hurting the radio (SPI, PSRAM, DSI):
    throughput collapses **while the CPU is idle**, and `read=` in the MJPEG log
    approaches 1000 ms/s — the task is waiting on the socket.
- **`WIFI_PS_NONE`** is mandatory: Wi-Fi power save causes massive RTP loss.
- **FreeRTOS tick at 1000 Hz**, as esp_hosted requests at boot. Sustained MJPEG
  went from a ceiling of ~980 KB/s to 1080-1165 KB/s.
- **L2 cache line must stay 64 B.** `CONFIG_CACHE_L2_CACHE_LINE_128B` speeds up
  JPEG decoding, but esp_hosted hard-codes a 64-byte DMA alignment: the SDIO
  link corrupts and the board reboots on "Failed to send data".
- **Do not pin lwIP's `tcpip` task to core 1.** Measured strictly worse: the
  `mjpeg` drain (core 0) and `tcpip` wake each other on every packet; splitting
  them turns every `recv()` into a cross-core round trip.
- **`C0 = 100 %` is a consequence of throughput, not the ceiling.** With
  `tcpip` moved, core 0 kept 26 % idle and throughput did not change by a byte.
- Per-core load is only reliable from the `CPU: C0=…% C1=…%` log line:
  `tcpip` and `sdio_*` have no affinity, so summing task percentages tells
  nothing.

## Memory

- **Code runs from PSRAM** (`CONFIG_SPIRAM_XIP_FROM_PSRAM`): in session core 0
  33 → 24 %, core 1 16 → 10 %, OTA 136 → 34 s. Side effect: flash/NVS writes no
  longer freeze the other core.
- **LVGL allocates in PSRAM** (`CONFIG_LV_USE_CUSTOM_MALLOC`,
  `components/bsp/lv_mem_psram.c`). With the clib allocator every object
  < 16 KB went to internal SRAM (~240 KB): free internal SRAM in session went
  from 20 KB to 248 KB. That RAM is what DTLS needs.
- **Internal SRAM is the scarce resource.** `esp_peer_open` returning −1 =
  mbedTLS out of internal memory during the DTLS handshake.
- Big buffers (> 4 KB, frames, audio) always in PSRAM.
- Any task touching FATFS or NVS needs ≥ 8 KB of stack (a 4 KB teardown task
  overflowed at 6.3 KB the day call history added SD + NVS writes to it).

## Audio and I2S

- **Never `vTaskDelete` a task that may be blocked in a driver.**
  `i2s_channel_read` holds the channel's binary semaphore for the whole read:
  killing the task leaves the channel dead, with the misleading message "The
  channel is not enabled" every ~1 s. Stop audio tasks cooperatively (atomic
  flag + "stopped" semaphore).
- Measuring an I2S cadence: flush the DMA ring first. At the first `read_pcm`
  it already holds ~180 ms of audio and returns in ~70 µs.
- Echo cancellation: only esp-sr **FD** modes are usable (FD_HIGH_PERF ~38 % of
  core 1 during a call); VOIP modes cost 60-450 % of a core on this silicon.
- Ringtone WAVs must be near **−1 dBFS**: the firmware never normalises.

## microSD, USB and the SDMMC controller

- esp_hosted claims the single SDMMC controller exclusively since IDF 6. The
  BSP overrides `host.init` and **never calls `host.deinit`**: releasing it
  would cut Wi-Fi (IDF issue #16233).
- **No SD I/O on a hot path**: it adds jitter to the MJPEG drain. The ringtone
  is preloaded at boot; the SD log writes bounded 4 KB chunks from its own task.
- USB mass-storage mode unmounts the card before exposing it (two owners of a
  FAT = corruption), and everything that could touch the card is inhibited
  meanwhile.
- **`CONFIG_TINYUSB_MSC_ENABLED` is a silent gate**: without it, MSC code links
  fine but never appears in the descriptors. `do_not_format = true` is
  mandatory, or the card is reformatted at enumeration.

## Front camera (OV02C10, MIPI-CSI)

Three conditions, each silent when missing:

- **An ISP processor in `bypass_isp` is required** even for raw frames: data
  crosses the ISP input module before the DMA bridge. Without it the sensor
  streams, CSI reports nothing, and no frame ever completes. Never call
  `esp_isp_enable()` on a bypass ISP (IDF 6.0.1 flips the state before
  refusing; the only ISP is then stuck until reboot).
- **Frame size must be a multiple of 64 bytes** (cache sync of the CSI fallback
  buffer): native 1288×728 RAW10 fails with `ESP_ERR_INVALID_ARG`, hence the
  forced 1280×720.
- **No line start/end packets** from this sensor configuration: if the ISP
  waits for them every frame is discarded.
- No format conversion in the bridge on pre-v3 silicon: RAW10 packed (5 bytes =
  4 pixels), Bayer GBRG. The camera does not affect Wi-Fi RSSI.

## ESP-IDF 6.0

- `espressif/esp_hosted` is pinned to **`~2.12`** in `main/idf_component.yml`.
  3.x rewrites the transport and requires matching C6 firmware.
- **`CONFIG_MBEDTLS_X509_CREATE_C=y` is mandatory**: mbedTLS 4 gates X.509
  writing behind it (default `n`); without it `esp_peer`'s DTLS-SRTP does not
  compile → no audio, no microphone.
- `json` and `mqtt` left ESP-IDF: they come from the registry
  (`espressif/cjson`, `espressif/mqtt`); cJSON registers as `espressif__cjson`.
- `libpeer_default.a` references an `esp_log` symbol that IDF 6 now defines
  itself. Do **not** add a stub (conflicting declaration).
- **picolibc**: `__bufio_put` writes one byte past a `FILE` buffer. The victim
  was the SD log's `fopen`'d file (128-byte heap buffer); contained with a
  `setvbuf` with slack. Re-test on every IDF upgrade.
- **Never `idf.py set-target`** (resets `sdkconfig`). Log level max = INFO
  (`CONFIG_LOG_MAXIMUM_LEVEL=3`): every `ESP_LOGD` is compiled out, which is
  what makes it free on hot paths.

## go2rtc side

- **Production must stay below delivery.** If ffmpeg produces more than the
  board receives, TCP queues it: audio runs ahead and the delay never recovers.
  `drop=0` does not mean all is well — it only counts frames overwritten in the
  mailbox (late rendering), not a slow link.
- go2rtc ≥ 1.9.15 needs `webrtc: ice_servers: []` + an IPv4 LAN filter (SDP
  answer 5.1 s > firmware timeout 4 s otherwise).
- Timestamp the MJPEG stream with the wall clock
  (`setpts=(RTCTIME-RTCSTART)/(TB*1000000),fps=15`), not `-r 15`: when a second
  client joined the source the RTP sequence jumped and ffmpeg duplicated the
  same frame at ~250 fps (frozen video, go2rtc at 120 % CPU).
- A door station has **one** talk channel. Any client that opens the ONVIF
  backchannel (seen: Home Assistant's ONVIF camera) silently takes it, and the
  station answers `404` to everyone else's `DESCRIBE`.
