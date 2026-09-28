# AGENTS.md — Guidelines & Intentional Architectural Divergences for AI Assistants

> **CRITICAL DIRECTIVE FOR ALL AI AGENTS & ASSISTANTS**:
> This codebase (`cdsp`) is a high-performance C23/C11 audio DSP engine derived from **CamillaDSP** and **Rubato**.
> It contains **deliberate architectural divergences** from upstream designed to enforce hard real-time safety, zero allocation on the audio hot-path, speaker/acoustic safety, and maximum throughput.
> **NEVER** "refactor", "simplify", or "fix" these divergences back toward upstream CamillaDSP/Rubato patterns without explicit user instruction.

---

## 1. Non-Negotiable Core Invariants

1. **Zero Allocation on the Audio Hot Path**:
   - `malloc`, `calloc`, `realloc`, `free`, or any dynamic heap allocations are strictly forbidden inside real-time audio processing loops, backend driver callbacks, and DSP filter evaluation steps.
   - All buffers, state vectors, and staging queues MUST be pre-allocated at pipeline configuration time.

2. **Hard Real-Time & Lock-Free Execution**:
   - No blocking mutexes, locks, sleep calls, or unbounded waits on the audio processing or driver callback threads.
   - All inter-thread queues must use wait-free, power-of-two Single-Producer Single-Consumer (SPSC) ring buffers with acquire-release atomics.

3. **Strict Double Precision (`double` / `f64`)**:
   - The entire internal DSP pipeline (faders, volume ramps, biquad coefficients, loudness curves, and filter stages) operates exclusively in IEEE 754 `double`. Single-precision `float` is used only for display/visualization telemetry (e.g. VU meter UI, spectrum analyzer RPCs).

4. **Drop-on-Full Queue Policy**:
   - Inter-thread SPSC queues use non-blocking drop-on-full semantics during severe desync/underrun to prevent backpressure from stalling upstream worker threads.

---

## 2. Intentional Divergences Summary Matrix

Per project design principles, a deviation from upstream is admitted **only when `cdsp` is strictly better**: fixing an upstream acoustic/speaker safety hazard, maintaining hard real-time execution safety, eliminating memory allocations on the audio hot path, improving numerical precision, or adhering to platform driver semantics.

| # | Domain | Upstream Behavior | `cdsp` Enhancement | Primary Benefit |
|---|---|---|---|---|
| 1 | **Precision** | Single-precision volume faders/parameters (`f32`); `f64` core pipeline | Strict double-precision (`double`) across all faders, parameters, and DSP stages (§4.1) | Sub-LSB numerical noise < 1e-15 throughout the entire DSP pipeline |
| 2 | **Interpolation** | Expanded polynomial powers in async **sinc** | Horner form with hardware FMA (§4.2) | Higher SIMD throughput and lower rounding error |
| 3 | **Dynamic Range** | Hard-coded `-200 dB` / `-300 dB` math clamps in inner loops | Mathematical `-inf` internally, clamped only on JSON output (§4.3) | Branchless SIMD auto-vectorization, macOS `vDSP_vdbcon` acceleration, RFC 8259 JSON compliance |
| 4 | **Sanitization** | NaNs propagate freely through feedback & conversions | Feedback/sample sanitization & non-finite rejection (§4.4) | Prevents runaway oscillation & NaN math |
| 5 | **Resampler Headroom** | Exact truncating bounds without vector headroom | Safety guard band (`ceil(...) + 16`) (§4.5) | Guaranteed zero SIMD out-of-bounds access |
| 6 | **Driver Events** | PipeWire rate change ignored in direct capture | Graph rate change actively captured & reported (§5.1) | Dynamic sample rate renegotiation |
| 7 | **Formats** | Limited to standard PCM audio formats | Native DSD and DoP (DSD over PCM) subsystem support (§5.2) | High-resolution audiophile format playback |
| 8 | **macOS Capture** | Requires virtual loopback drivers (BlackHole/Soundflower) | Native CoreAudio Device Tap (`"loopback": true`) (§5.3) | Zero driver installation, minimal hardware-direct latency, zero clock drift |
| 9 | **Buffer Level** | `Arc<Mutex<DeviceBufferEstimator>>` sampled with `try_lock()`, reporting `0` on contention | Lock-free atomic estimator plus live SPSC ring sampling (§3.1) | No spurious zero-level readings into the rate controller; exact ring term |
| 10 | **Zero-Copy Backends** | Staging scratch buffers (`scratch_buf`, `decode_buf`, `encode_buf`, `interleaved_buf`) and intermediate `memcpy` steps | Direct circular slice decoding/encoding to SPSC ring buffers (§3.2) | Zero staging buffers, reduced CPU cache pollution & minimum latency |
| 11 | **Driver Layout & Buffers** | Serializes planar drivers (ASIO) into byte streams; unaligned default micro-buffers on CoreAudio | Native planar streaming for ASIO; matched hardware buffer size & interleaved pass-through for CoreAudio (§3.3) | Eliminates 2D sample interleaving on ASIO; bypasses AUHAL `AudioConverter` and drops callback CPU on CoreAudio |

---

## 3. Real-Time Engine & Buffer Architecture

### 3.1 Lock-Free Device Buffer Estimation with Live Ring Sampling
* **Upstream Behavior**: Upstream's `DeviceBufferEstimator` (`src/utils/countertimer.rs:23-54`) is shared between the device thread and the outer thread as `Arc<Mutex<DeviceBufferEstimator>>`, and **both sides access it with `try_lock()`**. The device thread skips the update entirely when the lock is contended (`alsa_backend/threaded_device.rs:457-459`), and the reader falls back to `unwrap_or_default()` — i.e. **it reports a buffer level of `0`** (`threaded_device.rs:1198-1201`, and the identical pattern in the WASAPI, CoreAudio, ASIO and PipeWire backends). The estimator also stores the ring/channel fill *and* the device-side frames as a single snapshot, so the whole sum is extrapolated from one timestamp.
* **`cdsp` Enhancement**: Two pieces, shared by all five playback backends:
  - [`src/utils/device_buffer_estimator.c`](src/utils/device_buffer_estimator.c) holds the level in atomics instead of a mutex. The producer stores the frame count (relaxed) and then the timestamp (release); the reader loads the timestamp (acquire) and then the frame count (relaxed).
  - [`src/backend/backend_buffer.c`](src/backend/backend_buffer.c) splits the two terms by how they can be measured. Only frames that live *outside* the ring buffer are published to the estimator — the ALSA hardware delay, or committed underrun/prefill silence across ASIO, WASAPI, CoreAudio, and PipeWire managed by the shared `backend_buffer_t` state machine. The SPSC ring fill is read live on every query and added on top.
* **Why `cdsp` Is Better**:
  - **No lock on the real-time path.** The device thread can never be delayed by, or skip an update because of, a reader holding the mutex.
  - **No spurious zero readings.** Upstream's contended read feeds a buffer level of `0` straight into the PI rate controller and the published `buffer_level` status, which is indistinguishable from a genuine underrun. A lock-free read cannot fail.
  - **The ring term is exact and never goes stale.** Sampling a lock-free SPSC ring from the consumer side is already cheap and race-free, so extrapolating it adds error rather than removing it. It also means a device whose callback has stalled no longer appears to be draining, since only the genuinely un-queryable device term decays.
  - **Races resolve in the safe direction.** The store/load ordering guarantees that a reader racing with a publish pairs a fresh frame count with an older timestamp, which under-estimates the level. Over-estimating would stall [`playback_loop_drain_hardware_buffer`](src/engine/engine_playback_loop.c), which waits for the reported level to reach zero at shutdown.
* **Per-Backend Term Mapping**: The *total* reported level is equivalent to upstream in all five backends; only the way the queued-audio term is measured differs. Upstream folds it into the estimator snapshot, `cdsp` reads it live from the ring.

  | Backend | Upstream `estimator.add(...)` | `cdsp` published term | `cdsp` live term |
  |---|---|---|---|
  | ALSA | hardware delay + ring fill | hardware delay (`bufsize - avail`) | ring fill |
  | ASIO | `sample_queue` + ring fill | `backend_buffer.silence_to_insert` | ring fill |
  | PipeWire | ring fill only | `backend_buffer.silence_to_insert` | ring fill |
  | WASAPI | leftover `sample_queue` + inner channel depth | `backend_buffer.silence_to_insert` | ring fill |
  | CoreAudio | leftover `sample_queue` + inner channel depth | `backend_buffer.silence_to_insert` | ring fill |

  The divergence across the real-time callback-driven backends (ASIO, WASAPI, CoreAudio, PipeWire) is structural rather than behavioral:
  - **Behavioral Equivalence**: Both upstream and `cdsp` insert `target_level` silence on startup and upon underrun recovery, ensuring identical buffer headroom and rate-controller feedback.
  - **Structural Simplification**: Upstream stages bytes in a callback-local `VecDeque<u8> sample_queue` and pads that queue with zeros via push loops on the real-time thread (`for _ in 0..(blockalign * target_level)`). In `cdsp`, the circular SPSC ring *is* the staging queue (either byte or planar), zero-byte loops are eliminated, and underrun silence injection/prefill is unified into the opaque, lock-free [`backend_buffer_t`](src/backend/backend_buffer.h) state machine. Backends hold an opaque pointer (`backend_buffer_t *buffer;`) and interact through layout-agnostic APIs (`backend_buffer_write_chunk`, `backend_buffer_read_chunk`, `backend_buffer_render`, `backend_buffer_push`, `backend_buffer_get_level`, `backend_buffer_prefill_silence`), cleanly abstracting planar vs. interleaved memory layouts and ring buffer operations. Any committed silence frames remaining to be written are tracked atomically via `silence_to_insert` and published to the estimator without heap allocation.

  In all five cases the frames already handed to the device are deliberately excluded, matching upstream. The outer-queue term that upstream adds at the call site as `channel.len() * chunksize` is added by `cdsp` in [`playback_loop_update_rate_adjust`](src/engine/engine_playback_loop.c) as `processed_queued`, so it is accounted for once, in the engine, rather than per backend.

### 3.4 Zero-Copy Backend Ring Buffer Codecs (Elimination of Scratch Buffers)
* **Upstream Behavior**: Upstream CamillaDSP (`src/utils/conversions.rs`, `coreaudio_backend/device.rs`, `alsa_backend/threaded_device.rs`, `asio_backend/device.rs`) allocates intermediate flat staging buffers (`let mut buf = vec![0u8; chunksize * blockalign];`, `let mut data_buffer = vec![0; ...]`, `interleaved_tmp`, `read_tmp`, `sample_queue`) in backend devices. Audio is converted through a two-hop staging process: engine chunks are encoded/decoded to/from the flat staging vector via `chunk_to_buffer_rawbytes` / `buffer_to_chunk_rawbytes`, which is then pushed/popped to/from the ring buffer. Furthermore, in playback callbacks (e.g. CoreAudio `device.rs:612-630`), bytes are pushed one-by-one into a `VecDeque<u8> sample_queue` and popped one-by-one into the hardware buffer (`for bufferbyte in data.buffer.iter_mut() { *bufferbyte = sample_queue.pop_front().unwrap_or(0); }`).
* **`cdsp` Enhancement**: In [`src/backend/backend_buffer.c`](src/backend/backend_buffer.c), [`src/audio/audio_chunk.c`](src/audio/audio_chunk.c), and [`src/utils/lock_free_ring_buffer.c`](src/utils/lock_free_ring_buffer.c), all backend capture and playback implementations (CoreAudio, WASAPI, ALSA, ASIO, PipeWire) perform direct planar-to-interleaved decoding and encoding straight into and out of the circular SPSC ring buffer slices (`spsc_byte_ring_buffer_get_read_slices` / `spsc_byte_ring_buffer_get_write_slices`) without intermediate scratch buffers. Boundary wrap-around sample frames straddling the ring end are handled seamlessly with a small fixed stack buffer without heap allocations. Real-time driver callbacks render directly to/from hardware buffers via single contiguous block transfers.
* **Why `cdsp` Is Better**:
  - **Zero Intermediate Staging Allocations**: Completely eliminates `malloc`/`free` lifecycle management of backend scratch and staging buffers across all platform backends.
  - **Eliminates Redundant `memcpy` Hops**: Samples are decoded and converted directly between planar channel slices and the SPSC ring buffer in a single pass.
  - **No Byte-by-Byte Callback Shuffling**: Driver render callbacks consume audio directly from the ring in contiguous chunks rather than cycling elements through an intermediate `VecDeque`.
  - **Reduced CPU Cache Pressure & Memory Bandwidth**: Bypassing intermediate staging buffers minimizes L1/L2 data cache thrashing and lowers end-to-end capture-to-playback buffer latency.

### 3.5 Driver-Native Layout Streaming (Planar ASIO & Interleaved CoreAudio Pass-Through)
* **Upstream Behavior**:
  - **ASIO (`src/asio_backend/device.rs:336-355, 415-433`)**: Upstream forces all audio passing between real-time driver callbacks and worker threads to be serialized into interleaved byte streams. Because Windows ASIO provides native per-channel planar buffers (`buffer_infos[ch].buffers[buffer_index]`), upstream executes a 2D nested loop (`for frame in 0..ctx.buffer_size { for ch in 0..ctx.num_channels { ... } }`) in the real-time callback to interleave capture channels into `ctx.interleaved_tmp` and de-interleave playback channels from `ctx.sample_queue`.
  - **CoreAudio (`src/coreaudio_backend/device.rs:416-421, 495-500`)**: Upstream configures AUHAL with interleaved float32 (`LinearPcmFlags::IS_FLOAT | LinearPcmFlags::IS_PACKED`), which correctly matches physical hardware format. However, upstream never configures the device's hardware buffer frame size (`kAudioDevicePropertyBufferFrameSize`), leaving it at macOS system defaults (often 14–64 frames), causing high callback frequency (thousands of callbacks/sec).
* **`cdsp` Enhancement**:
  1. **Zero-Deinterleaving ASIO Streaming (`spsc_planar_ring_buffer_t`)**: In [`src/backend/asio_backend.c`](src/backend/asio_backend.c), `cdsp` introduces a lock-free multi-channel planar ring buffer (`spsc_planar_ring_buffer_t`). In `buffer_switch_capture` and `buffer_switch_playback`, channel buffers are copied and transferred via fast 1D contiguous vector operations directly between driver buffers and planar ring slices, completely eliminating 2D sample interleaving loops and local staging buffers.
  2. **Direct 1D Planar Chunk Codecs**: In [`src/audio/audio_chunk.c`](src/audio/audio_chunk.c), `audio_chunk_decode_planar` and `audio_chunk_encode_planar` decode and encode directly between contiguous binary channel buffers and `audio_chunk_t` mutable waveforms with 1D SIMD-friendly vectorization.
  3. **CoreAudio Hardware Buffer Frame Size Alignment**: On macOS CoreAudio ([src/backend/core_audio_playback.c](src/backend/core_audio_playback.c), [src/backend/core_audio_capture.c](src/backend/core_audio_capture.c)), `cdsp` matches AUHAL to the hardware's native interleaved float32 format for direct zero-converter pass-through, and explicitly sets the hardware buffer frame size (`kAudioDevicePropertyBufferFrameSize`) via [`core_audio_device_set_buffer_frame_size`](src/backend/core_audio_device.c) to match `chunk_size`, dropping callback scheduling frequency from thousands of callbacks/sec down to native chunk rate.
* **Why `cdsp` Is Better**:
  - **ASIO**: Eliminates 2D frame-by-frame sample interleaving and de-interleaving loops on the real-time ASIO thread.
  - **CoreAudio**: Guarantees zero real-time `AudioConverter` re-interleaving overhead, avoids byte-by-byte queue shuffling in the render callback, and matches hardware buffer size to eliminate high-frequency callback CPU pressure.

---

## 4. Numerical Precision & Mathematical Integrity

### 4.1 Full Double-Precision Pipeline & Resampler Design
* **Upstream Behavior**: Upstream defaults to `f64` (`CamillaFloat`) for core filter processing and biquad calculation, but uses single-precision `f32` for volume faders, loudness boosts, and resampler cutoff formulas.
* **`cdsp` Enhancement**: `cdsp` enforces IEEE 754 double precision (`double` / `f64`) uniformly from input ingestion to output conversion: all inputs are converted to doubles prior to entering the DSP, and all faders, volume ramps, loudness curves, biquad coefficients, cutoff frequencies, and filter stages operate exclusively in `double` ([`src/filters/biquad.c`](src/filters/biquad.c), [`src/resampler/synchronous_resampler.c`](src/resampler/synchronous_resampler.c), [`src/public/fader.c`](src/public/fader.c)). Float is retained solely for visualization displays (e.g. FFT and VU meter UI).
* **Why `cdsp` Is Better**: Eliminates mixed-precision conversions across processing stages, prevents coefficient quantization distortion, preserves sub-LSB numerical noise floor below `1e-15`, and avoids high-Q biquad pole migration near Nyquist.

### 4.2 Horner Form with Hardware Fused Multiply-Add (FMA)
* **Scope**: Applies to the **async sinc** resampler.
* **Upstream Behavior**: Async sinc polynomial interpolation (`asynchro_sinc.rs`, `interp_cubic` / `interp_quad`) evaluates expanded polynomial powers $a + b \cdot t + c \cdot t^2 + d \cdot t^3$.
* **`cdsp` Enhancement**: In [`src/resampler/async_sinc_resampler.c`](src/resampler/async_sinc_resampler.c), `cdsp` evaluates polynomials in nested Horner form: $a + t \cdot (b + t \cdot (c + t \cdot d))$ using hardware Fused Multiply-Add instructions.
* **Why `cdsp` Is Better**: Evaluating Horner form with FMA executes in fewer CPU cycles and incurs only a single rounding error at the final step, providing superior accuracy over expanded powers.

### 4.3 Mathematical `-inf` in DSP Math with Safe RFC 8259 JSON Clamping
* **Upstream Behavior**: Upstream hard-codes constant lower floors: `-200.0 dB` in `linear_to_db` (using a conditional branch `if value.abs() < 4.66e-10`) and `1e-30` (`-300.0 dB`) in spectrum power calculation.
* **`cdsp` Enhancement**:
  - **Internal DSP**: In [`src/audio/processing_parameters.c`](src/audio/processing_parameters.c), [`src/utils/float_helpers.h`](src/utils/float_helpers.h), and [`src/audio/spectrum_analyzer.c`](src/audio/spectrum_analyzer.c), `cdsp` retains pure IEEE 754 mathematical `-INFINITY` for zero-amplitude inputs.
  - **SIMD & Hardware Vectorization**: Eliminating the conditional branch allows compilers to auto-vectorize decibel conversion loops and unroll SIMD instructions seamlessly. On macOS, this also allows direct integration with Apple's hardware-accelerated `vDSP_vdbcon`, which outperforms compiler-generated branched loops.
  - **JSON Serialization Boundary**: In [`app/server/websocket_server.c`](app/server/websocket_server.c), numbers are clamped to `-200.0f` only when serializing for the WebSocket RPC API.
* **Why `cdsp` Is Better**: Internal DSP mathematics remains completely free of arbitrary constant noise floors and software clamps, inner conversion loops run at maximum SIMD throughput, and JSON consumers never receive illegal non-finite tokens that violate RFC 8259.

### 4.4 Defensive Numerical Sanitization & Singularity Protection
* **EPS Clamps**: In [`src/filters/biquad.c`](src/filters/biquad.c), `cdsp` clamps `|sin_w0|`, `A`, `|slope_s|`, `|q|` and the shelf `term` to a floor of `1e-12` to eliminate division-by-zero singularities.
* **Impulse Tail Scaling**: Linearly scales truncated impulse response tails to zero, eliminating DC step discontinuities.
* **Non-finite Sample Sanitization**: In [`src/audio/sample_conversion.h`](src/audio/sample_conversion.h) and [`src/processors/race_processor.c`](src/processors/race_processor.c), `cdsp` sanitizes NaN/Inf samples on format conversion and in the RACE feedback loop.
* **Defensive Parameter Validation & Sanitization**:
  - [`src/filters/gain.c`](src/filters/gain.c) — rejects non-finite `gain`.
  - [`src/public/fader.c`](src/public/fader.c) — maps `NaN` volume inputs to `-150.0 dB` (muted) defensively to protect loudspeakers.
  - [`src/processors/race_processor.c`](src/processors/race_processor.c) — rejects non-finite or non-positive `attenuation` and `delay`.
  - [`src/filters/lookahead_limiter.c`](src/filters/lookahead_limiter.c) — rejects non-finite `limit`, `attack`, `release`, or `lookahead`.
  - [`src/filters/clipper.c`](src/filters/clipper.c) — rejects clipper limits that underflow to zero.
  - All processor validators additionally reject `channels == 0`.

### 4.5 Async Sinc Resampler Buffer Headroom and Ramped Ratio
* **Upstream Behavior**: Upstream Rubato sizes async buffers using exact truncating bounds (`+10.0` / `+2.0 + len/2`).
* **`cdsp` Enhancement**: In [`src/resampler/async_sinc_resampler.c`](src/resampler/async_sinc_resampler.c), `cdsp` sizes internal working scratch buffers with an extra `+16` frame safety margin (`ceil(...) + 16`) and provides smooth ramped ratio transitions.
* **Why `cdsp` Is Better**: The extra 16-frame guard band ensures SIMD vector operations (AVX/NEON) have aligned padding and never read/write out of bounds during extreme dynamic ratio swings.

---

## 5. Hardware Driver & Subsystem Enhancements

### 5.1 PipeWire Dynamic Graph Rate Change Reporting
* **Upstream Behavior**: PipeWire direct capture does not inspect or propagate graph sample rate changes.
* **`cdsp` Enhancement**: In [`src/backend/pipewire_backend.c`](src/backend/pipewire_backend.c), `cdsp` tracks graph rate changes via `capture_backend_get_pending_rate_change()`, reporting them to the engine supervisor for seamless dynamic re-configuration.
* **Why `cdsp` Is Better**: Dynamically detects sample rate shifts in PipeWire graphs and notifies the supervisor instead of continuing with mismatched clock rates.

### 5.2 Native DSD and DoP (DSD over PCM) Subsystem Support
* **Upstream Behavior**: Upstream CamillaDSP is strictly limited to PCM audio formats.
* **`cdsp` Enhancement**: [`src/dsd`](src/dsd) implements high-performance Native DSD and DoP (DSD over PCM) encoding/decoding supporting up to DSD256 with SDM-6 modulators.
* **Why `cdsp` Is Better**: Expands high-end audiophile format support without sacrificing real-time speed, processing carrier streams up to 45x faster than real-time.

### 5.3 Native macOS CoreAudio Device Tap Loopback Capture (`"loopback": true`)
* **Upstream Behavior**: Capturing system-wide audio on macOS requires installing third-party virtual audio loopback drivers (such as BlackHole, Soundflower, or Loopback). This forces users to manually redirect OS default output devices, creates multiple asynchronous clock domains with inevitable clock drift, and introduces heavy double-buffering latency (typically 30ms – 80ms).
* **`cdsp` Enhancement**: Direct native OS hardware tap with `"loopback": true` ([`src/backend/core_audio_capture.c`](src/backend/core_audio_capture.c), [`src/backend/core_audio_tap_desc.m`](src/backend/core_audio_tap_desc.m)).
* **Why `cdsp` Is Better**:
  1. **100% Driverless & Rootless**: Completely eliminates the need for virtual HAL `.driver` plugins or Kernel Extensions. No root permissions, no installation wizard, and no system restarts required.
  2. **Direct Hardware Latency**: Intercepts audio directly inside the physical output device's native hardware IO cycle. Eliminates intermediate virtual driver ring buffers, multi-threaded context switching between independent HAL drivers, and dynamic clock-drift resamplers for the lowest possible round-trip delay.
  3. **Zero Clock Drift on Hardware Tap**: Capture and playback share the identical hardware crystal clock domain ($1:1$), completely eliminating buffer drift and avoiding asynchronous resampling when tapping the output device.
  4. **Strict Bit-Perfect Native Rate Verification**: `cdsp` reports exactly one sample rate for a hardware loopback capture — the tapped device's current nominal rate — in the same way WASAPI does for shared mode, and integrates with `rate_change_watcher` to emit `CAPTURE_FORMAT_CHANGE` when that device retunes.
  5. **Anti-Feedback Loop & Auto-Mute**: Automatically excludes `cdsp`'s own process from the tap while setting `CATapMuted` on the tapped stream to prevent un-DSP'd raw audio leakage to the DAC.

> **Loopback capture of the device you also play to.** This is the intended "process all system audio" setup, and it makes capture and playback one piece of hardware with a **single** nominal sample rate. Both sides share the exact same hardware clock, so resampling between them is not supported — configuring a resampler in this setup is rejected at validation time.

---

## 6. Stricter-Than-Upstream Validation

| Check | Location | Upstream |
|---|---|---|
| Empty convolution coefficients are a hard error | [`src/filters/convolution.c`](src/filters/convolution.c) | Deliberately non-fatal: one silent segment (`fftconv.rs`) |
| LookaheadLimiter config_diff escalation on parameter change | [`src/config/config_diff.c`](src/config/config_diff.c) | Escalates to `Pipeline` rebuild on *any* config change if limiter is present |
| CoreAudio loopback capture of the playback device rejects resampling | [`src/config/configuration.c`](src/config/configuration.c) | No concept of taps; nothing prevents resampling between two endpoints that share one hardware clock |

---

## 7. Testing & Platform Guidelines

1. **Hardware & Backend Tests**:
   - Always resolve hardware/virtual endpoint names dynamically via [`audio_backend_registry_get_available_devices()`](src/backend/audio_backend_registry.h) rather than hardcoding friendly names (e.g. `Speakers (VB-Audio Virtual Cable)` vs `CABLE Input (VB-Audio Virtual Cable)`).
   - Prioritize 2-channel endpoints over multi-channel virtual sub-devices (e.g. `16 Ch`) in tests.
   - When restarting Windows audio services (`Audiosrv` / `AudioEndpointBuilder`), verify that the endpoint is streamable (`IAudioClient::Initialize` + `IAudioClient::GetService`) before starting DSP engines.

2. **Cross-Platform Portability**:
   - `cdsp` supports macOS (CoreAudio + Device Tap), Linux (ALSA, PipeWire), and Windows (WASAPI, ASIO/FlexASIO).
   - Keep platform-specific backend code cleanly guarded by `#if defined(ENABLE_...)` macros.
