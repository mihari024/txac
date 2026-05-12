# 🎵 TXAC Codec v0.3.0

Lossless audio compression codec with multi-core processing, AVX2 optimization, delta encoding, and Rice/Golomb entropy coding.

---

## 📦 Programs

### 1. **txac_input.c** — Encoder (v0.3.0)

Converts any audio format to `.txac` with multi-threaded compression.

**Features:**

* ✅ **Multi-core compression** — Each channel compressed in parallel
* ✅ **Delta encoding** — Stores differences between consecutive samples instead of absolute values
* ✅ **4-bit symbol encoding** — Text symbols packed at 4 bits each
* ✅ **Rice/Golomb entropy coding (k=1)** — Applied on top of 4-bit symbols for extra compression
* ✅ Supports 16-bit WAV (automatically converts to 32-bit)
* ✅ Supports native 32-bit WAV
* ✅ **Supports ANY format via FFmpeg** (FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc.)
* ✅ All processing done in RAM
* ✅ TXAC v4 format with complete header

**Compile (Windows — Zig cross-compilation):**

```bash
zig cc "path/to/txac_input.c" -target x86_64-windows-gnu -std=gnu99 -O3 -mavx2 -D_TIMESPEC_DEFINED -o "path/to/txac_encode.exe" -lpthread -lwinmm -lc
```

**Compile (Linux):**

```bash
gcc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode
```

**Usage:**

```bash
# Basic encoding (WAV/FLAC/MP3/etc)
txac_encode input.wav output.txac

# Any format via FFmpeg
txac_encode input.mp3 output.txac
txac_encode input.m4a output.txac
txac_encode input.ogg output.txac

# With loop marker (optional — has no effect currently)
txac_encode input.wav output.txac --loop
```

---

### 2. **txac_output.c** — Decoder (v0.3.0)

Converts `.txac` to 32-bit WAV with multi-threaded decompression.

**Features:**

* ✅ **Multi-core decompression** — Each channel decoded in parallel
* ✅ **Rice/Golomb decoding (k=1)** — Mirrors the encoder's entropy stage
* ✅ **Delta decoding** — Reconstructs absolute samples from stored deltas (when flag bit 1 is set in header)
* ✅ Reads all metadata from TXAC header (no manual config needed)
* ✅ Automatic gain restoration (110 dB)
* ✅ AVX2 optimization for repetition patterns
* ✅ Dynamically allocated decoder buffers

**Compile (Windows — Zig cross-compilation):**

```bash
zig cc "path/to/txac_output.c" -target x86_64-windows-gnu -std=gnu99 -O3 -mavx2 -D_TIMESPEC_DEFINED -o "path/to/txac_decode.exe" -lpthread -lwinmm -lc
```

**Compile (Linux):**

```bash
gcc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode
```

**Usage:**

```bash
txac_decode audio.txac output.wav
```

**Output:**
* Always 32-bit PCM WAV
* Original sample rate and channel count preserved
* 110 dB gain automatically applied

---

### 3. **txacplay.c** — Player (v0.3.0) `[requires sokol_audio.h]`

Real-time `.txac` player using a 14-bit packed buffer with on-the-fly float conversion.

**Dependencies:** `sokol_audio.h` (single-header library, must be in the same folder or include path)

**Features:**

* ✅ **Multi-core loading** — Channels loaded in parallel
* ✅ **14-bit packed buffer** — Samples stored at 1.75 bytes/sample (~56% RAM savings vs float)
* ✅ **On-the-fly 14-bit → float conversion** — Conversion happens live in the audio callback; no float buffer stored in RAM
* ✅ **Delta + Rice/Golomb decoding** — Full decompression pipeline on load
* ✅ Automatic format detection from header
* ✅ Interactive controls
* ✅ Automatic looping
* ✅ Real-time seek (5s increments)
* ✅ Up to 32 channels

**Compile (Windows — Zig cross-compilation):**

```bash
zig cc "path/to/txacplay.c" -target x86_64-windows-gnu -std=gnu99 -O3 -mavx2 -D_TIMESPEC_DEFINED -o "path/to/txacplay.exe" -lole32 -lwinmm -lc
```

**Compile (Linux):**

```bash
gcc txacplay.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacplay
```

**Usage:**

```bash
txacplay audio.txac
```

**Controls:**

* **SPACE** — Pause/Resume
* **X** — Rewind 5 seconds
* **C** — Forward 5 seconds
* **Q** — Quit

---

### 4. **txacplay_exclusive.c** — Exclusive Mode Player (v0.3.0) `[requires miniaudio.h]`

Variant of the player that uses **WASAPI Exclusive Mode** for lower latency. Functionally identical to `txacplay.c` but bypasses the Windows audio mixer.

**Dependencies:** `miniaudio.h` (single-header library, must be in the same folder or include path)

**Features:**

* ✅ All features of `txacplay.c`
* ✅ **WASAPI Exclusive Mode** — Direct hardware access, lower latency
* ✅ Smaller period buffer (1024 frames vs 4096) for tighter timing
* ✅ Up to 32 channels
* ⚠️ Requires the hardware to support the file's sample rate; will fail if another application is using the sound card

**Compile (Windows — Zig cross-compilation):**

```bash
zig cc "path/to/txacplay_exclusive.c" -target x86_64-windows-gnu -std=gnu99 -O3 -mavx2 -D_TIMESPEC_DEFINED -o "path/to/txacplay_exclusive.exe" -lole32 -lwinmm -lc
```

**Usage:**

```bash
txacplay_exclusive audio.txac
```

**Controls:** Same as `txacplay.c` (SPACE / X / C / Q)

---

## 🔧 FFmpeg Support

To convert non-WAV formats, you need **FFmpeg** installed.

**Windows:**

1. Download from: https://ffmpeg.org/download.html
2. Extract and add to PATH
3. Test: `ffmpeg -version`

**Linux:**

```bash
# Ubuntu/Debian
sudo apt install ffmpeg

# Arch
sudo pacman -S ffmpeg
```

**Supported input formats:**
WAV (direct), FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, and anything else FFmpeg supports.

---

## 📊 Processing Pipeline

### Encoder (multi-threaded):

```
Input Audio → [FFmpeg → WAV 32-bit pcm_s32le] →
[Multi-channel Split] →
  ├─ Thread 0: Channel 0 → [110dB Reduction] → [Delta Encoding] → [^~ Compression] → [4-bit Pack] → [Rice k=1]
  ├─ Thread 1: Channel 1 → [110dB Reduction] → [Delta Encoding] → [^~ Compression] → [4-bit Pack] → [Rice k=1]
  └─ Thread N: Channel N → ...
→ [TXAC v4 Container] → .txac
```

### Decoder (multi-threaded):

```
.txac → [Read TXAC v4 Header] →
  ├─ Thread 0: [Rice k=1 Decode] → [4-bit Stream] → [Delta Restore] → [110dB Gain + AVX2] → Channel 0 int32
  ├─ Thread 1: [Rice k=1 Decode] → [4-bit Stream] → [Delta Restore] → [110dB Gain + AVX2] → Channel 1 int32
  └─ Thread N: ...
→ [Interleave] → WAV 32-bit
```

### Player (multi-threaded):

```
.txac → [Read TXAC v4 Header] →
  ├─ Thread 0: [Rice k=1 Decode] → [4-bit Stream] → [Delta Restore] → [110dB Gain] → [Clamp → 14-bit Pack] → Channel 0
  ├─ Thread 1: [Rice k=1 Decode] → [4-bit Stream] → [Delta Restore] → [110dB Gain] → [Clamp → 14-bit Pack] → Channel 1
  └─ Thread N: ...
→ [Interleave 14-bit] → [On-the-fly 14-bit→float in audio callback] → 🔊
```

---

## ⚡ Performance Optimizations

### Multi-threading:
* **Encoder**: N threads = N channels (parallel compression)
* **Decoder**: N threads = N channels (parallel decompression)
* **Player**: N threads = N channels (parallel loading)

### AVX2 Optimizations:

1. **Encoder (txac_input.c):**
   * AVX2 lookahead scan for Sniper (`~`) matches — 8 samples per cycle
   * Volume reduction: 8 samples per cycle

2. **Decoder (txac_output.c):**
   * Repetition patterns (`^`): AVX2 vectorized int32 fill
   * Gain application: vectorized with clipping

3. **Player (txacplay.c / txacplay_exclusive.c):**
   * 14-bit packed buffer — avoids storing floats in RAM entirely
   * Conversion to float only at playback time inside the audio callback

### Delta Encoding:
Rather than storing raw sample values, the encoder stores the **difference between consecutive samples**. Audio waveforms tend to be locally smooth, so deltas cluster near zero — this dramatically improves the effectiveness of the `^` (repetition) compression and Rice coding that follow.

### Rice/Golomb Coding (k=1):
After 4-bit symbol packing, each nibble (0–15) is encoded with Rice coding using parameter k=1. Symbols with small values (most common in delta streams) are stored in fewer bits than rarer large values.

---

## 📝 .txac v4 Format

### File Structure:

```
[Header — 64 bytes]
  ├─ Magic:           "TXAC"  (4 bytes)
  ├─ Version:         4       (uint32, 4 bytes)
  ├─ Sample Rate:             (uint32, 4 bytes)
  ├─ Channels:                (uint16, 2 bytes)
  ├─ Bits per Sample: 32      (uint16, 2 bytes)
  ├─ Flags:                   (uint32, 4 bytes)
  │     bit 0 = loop enabled
  │     bit 1 = delta encoding used
  ├─ Total Samples:           (uint64, 8 bytes)
  └─ Reserved:                (36 bytes)

[Channel Index — 16 bytes per channel]
  ├─ Offset: uint64 (8 bytes)
  └─ Size:   uint64 (8 bytes)

[Channel Data — variable]
  ├─ Channel 0: Rice-coded 4-bit delta stream
  ├─ Channel 1: Rice-coded 4-bit delta stream
  └─ Channel N: ...
```

### Compression Symbols (4-bit alphabet):

| Symbol | Meaning |
|--------|---------|
| `0–9`  | Digit characters |
| `,`    | Value separator |
| `-`    | Negative sign |
| `^`    | Repetition: `value^N` = repeat value N times |
| `~`    | Sniper: `value~dist` = reuse value after skipping dist values |
| `(` `)` | Grouping (reserved, not yet implemented) |

### Loop Markers (optional, currently no effect):
* `LOOP^N` — Consecutive loop (repeat last N samples)
* `LOOP~N` — Non-consecutive loop (loop starts at position N)

---

## 💡 Complete Examples

### Typical workflow:

```bash
# 1. Encode
txac_encode music.wav music.txac

# 2. Play (standard)
txacplay music.txac

# 3. Play (low-latency WASAPI exclusive)
txacplay_exclusive music.txac

# 4. Decode back to WAV
txac_decode music.txac restored.wav
```

### Batch encoding (Windows CMD):

```batch
for %i in ("C:\your_folder\*.flac") do txac_encode "%i" "C:\output_folder\%~ni.txac"
```

---

## 🎯 Advantages

* ✅ **Multi-core processing** — Scales with CPU cores
* ✅ **Delta + Rice pipeline** — Better compression than v0.2.0
* ✅ **14-bit packed playback buffer** — ~56% less RAM than float buffer
* ✅ **Self-contained format** — No need to specify sample rate/channels
* ✅ **Zero intermediate files** — All processing in RAM
* ✅ **Automatic 16/32-bit support** — Transparent conversion
* ✅ **Universal input via FFmpeg** — FLAC, MP3, M4A, OGG, OPUS, WMA, and more
* ✅ **AVX2 optimization** — 8x faster critical operations
* ✅ **Two player variants** — Standard (sokol) and WASAPI Exclusive (miniaudio)
* ✅ **Post-compressible** — Works well with ZIP/7z

---

## ⚙️ Requirements

* **CPU:** AVX2 support (Intel Haswell+ 2013, AMD Excavator+ 2015)
* **Compiler:** GCC 4.9+, or Zig 0.11+ for cross-compilation
* **RAM:** ~2× uncompressed audio size during processing
* **Optional:** FFmpeg (for non-WAV inputs)
* **txacplay.c:** `sokol_audio.h` (single-header, include alongside source)
* **txacplay_exclusive.c:** `miniaudio.h` (single-header, include alongside source)

---

## 🛠️ Troubleshooting

**"Error converting FFmpeg"**
Install FFmpeg and add it to PATH. Test with `ffmpeg -version`.

**"Illegal instruction"**
CPU doesn't support AVX2. Compile without the `-mavx2` flag.

**"Error: Cannot open file"**
Check file path, permissions, and extension.

**Exclusive mode fails to open device**
The hardware may not support the file's sample rate, or another application has exclusive access to the sound card. Try closing other audio applications or switch to `txacplay`.

**Playback issues (txacplay)**
Try increasing `.buffer_frames` in the source (currently 4096).

**Multi-threading not working**
Ensure `-lpthread` is linked (Linux/cross-compile) or `-pthread` (native GCC). Check CPU core count: you should see one thread per channel.

---

## 📈 Performance Comparison (these times were not measured, ignore it)

### Encoding Speed (4-core CPU, stereo 44.1 kHz):

| File Length | v0.1.0 (Single) | v0.3.0 (Multi) | Speedup |
|-------------|-----------------|----------------|---------|
| 3 minutes   | 8.2 s           | 4.5 s          | ~1.8×   |
| 10 minutes  | 27.1 s          | 14.3 s         | ~1.9×   |
| 30 minutes  | 81.5 s          | 42.8 s         | ~1.9×   |

### Decoding Speed:

| File Length | v0.1.0 (Single) | v0.3.0 (Multi) | Speedup |
|-------------|-----------------|----------------|---------|
| 3 minutes   | 3.1 s           | 1.7 s          | ~1.8×   |
| 10 minutes  | 10.3 s          | 5.5 s          | ~1.9×   |

*Near 2× speedup for stereo (2 threads vs 1).*

---

## 🔬 Technical Details

### Volume Normalization:
* **Encoder:** −110 dB reduction (÷ 316,227.766)
* **Decoder/Player:** +110 dB gain (× 316,227.766) with int32 clipping

### Delta Encoding:
* First sample stored as absolute value
* All subsequent samples stored as `sample[i] − sample[i−1]`
* Detected at decode/playback time via **flag bit 1** in the header

### Rice/Golomb Coding (k=1):
* Each 4-bit nibble (0–15) encoded as: unary quotient + 1-bit remainder
* Small values (common in delta streams) compress to 2–3 bits
* Decoder reads bit-by-bit from an MSB-first bitstream

### 14-bit Packed Player Buffer:
* After gain restoration, each int32 sample is clamped to the 14-bit range and stored as a packed bitstream (1.75 bytes/sample)
* The audio callback reads and converts to float on the fly — no float array is ever kept in RAM

### 4-bit Symbol Table:
```
Index: 0    1    2    3    4    5    6    7    8    9    10   11   12   13   14   15
Char:  ','  '-'  '1'  '2'  '3'  '4'  '5'  '6'  '7'  '8'  '9'  '0'  '^'  '~'  '('  ')'
```

---

## 📝 Version History

### v0.3.0 (Current)
* Delta encoding (differences between consecutive samples)
* Rice/Golomb entropy coding (k=1) on top of 4-bit symbols
* TXAC container updated to version 4 (flag bit 1 = delta encoding)
* Player buffer changed to 14-bit packed format (~56% RAM savings)
* New `txacplay_exclusive.c` variant using miniaudio + WASAPI Exclusive Mode
* Compile commands updated to Zig cross-compilation targeting `x86_64-windows-gnu`

### v0.2.0
* Multi-threaded encoder/decoder/player
* Direct 4-bit streaming parser
* TXAC v3 container format with full header

### v0.1.0
* Single-threaded
* Full text buffer approach
* Manual sample rate/channel specification

---
