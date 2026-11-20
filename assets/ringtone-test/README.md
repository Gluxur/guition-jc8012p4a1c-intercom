# Ringtones and ringtone test vectors

Custom ringtones are WAV files read from the **root of the microSD card**
(`media/ringtone_sd.cpp`) and picked on the Settings page. The firmware expects
**8 kHz, mono, 16-bit PCM**, at most 30 s.

## Ready-to-use ringtones

All at the expected format and normalised to **−1 dBFS**. Each ends with a bit
of silence: the ring loops until answered (or 30 s), and the silent tail avoids
a frantic back-to-back loop.

| File | Length | Content |
|---|---|---|
| `ring_fun_cartoon.wav` | 2.6 s | Spring boing, sliding whistle, second boing, pop |
| `ring_fun_chiptune.wav` | 3.3 s | Arpeggiated 8-bit melody + bass, square waves |
| `ring_fun_derail.wav` | 3.2 s | Classic ding-dong going out of tune and collapsing |
| `ring_fun_klaxon.wav` | 2.4 s | Clown horn + three duck quacks |
| `ring_fun_ufo.wav` | 4.2 s | Flying saucer: vibrato theremin, then landing |
| `ring_fun_robot.wav` | 2.3 s | Chatty droid: gliding square beeps + final trill |
| `ring_fun_cuckoo.wav` | 3.3 s | Wooden tick-tock then three "cuck-oo" (G5 → E5) |
| `ring_fun_bubble.wav` | 3.2 s | Eleven rising bubble pops + gurgle |
| `ring_fun_arcade.wav` | 2.2 s | Three falling "pew" + bonus-coin arpeggio |
| `ring_fun_saloon.wav` | 3.1 s | Honky-tonk ragtime piano lick |
| `ring_fun_frog.wav` | 3.3 s | Six croaks + splash |

All were synthesised by script (no external source, no third-party licence):
phase-integrated oscillators, exponential envelopes and **band-limited**
square/saw waves — at 8 kHz Nyquist is 4 kHz, and a naive square wave would
alias into a metallic hiss.

⚠️ The Settings drop-down is capped at `ringtone_sd::kMaxFiles` = **16 files**.

## Test vectors

For **manual** validation of the whole chain on the device, SD card included.
Header decoding itself is covered on host by `test/test_wav_parse.cpp`.

| File | Format | Expected behaviour |
|---|---|---|
| `ring_ok.wav` | 8 kHz mono PCM16, 3.0 s | Loaded |
| `ring_44k.wav` | 44.1 kHz mono PCM16 | Rejected ("8000 Hz required"), falls back to the built-in ding-dong |
| `ring_long.wav` | 8 kHz mono PCM16, 3 min | Loaded but truncated to 30 s |

## Loudness — the trap

The firmware **does not normalise**: 100 % on the volume slider is a fixed
ceiling (0.244 of full scale), nothing more. A file recorded quietly stays
quiet, by design. A file at −20 dBFS is effectively inaudible on the device.

Check a ringtone before copying it to the card:

```bash
ffmpeg -i ring.wav -af volumedetect -f null -   # look at max_volume
```

Aim for **−1 dBFS**. Convert any file to the expected format with:

```bash
ffmpeg -i input.mp3 -ac 1 -ar 8000 -sample_fmt s16 -af "loudnorm,volume=0dB" ring.wav
```
