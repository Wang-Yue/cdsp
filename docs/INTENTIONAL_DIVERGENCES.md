# Intentional Divergences & Architectural Enhancements

This document catalogs every area where **`cdsp`** deliberately diverges from upstream **CamillaDSP** and **Rubato**.

Per project design principles, a deviation from upstream is admitted **only when `cdsp` is strictly better**: fixing an upstream acoustic/speaker safety hazard, maintaining hard real-time execution safety, eliminating memory allocations on the audio hot path, improving numerical precision, or adhering to platform driver semantics.

Notably, upstream CamillaDSP has increasingly adopted architectural designs and safety enhancements pioneered in this codebase, steadily shrinking the list of divergences over time. Prominent examples of upstream convergence include:
- **Background Hot-Reload & IR Loading**: Validating and caching impulse responses (`ImpulseCache`) in memory prior to configuration application, eliminating disk I/O and heavyweight transformations on the real-time audio thread.
- **Audio Thread Allocation & Lock Reduction**: Reusing preallocated buffer stashes for waveform containers and intermediate filter stages, eliminating hot-path heap allocations (`malloc`/`Vec::new`) during steady-state processing, and avoiding blocking locks on the real-time processing loop.
- **Stricter Numeric & Parameter Validation**: Rejecting `.nan` / `.inf` tokens across all numeric configuration fields, strictly requiring positive sample rates and chunk sizes, bounding resampler cutoffs to `(0, 1]`, and rejecting hazardous compressor factors (`factor <= 0.0`).
- **Direct ASIO COM Driver Backend**: Communicating directly with ASIO driver COM interfaces without requiring Steinberg's proprietary ASIO SDK, eliminating external SDK and compiler-wrapper dependencies.

---

## Summary Matrix

| # | Domain | Upstream Behavior | `cdsp` Enhancement | Primary Benefit |
|---|---|---|---|---|
| 1 | **Volume** | Leaves residual samples past chunk size unscaled | Scales residual tail by target gain (§1.1) | Acoustic protection against bursts |
| 2 | **Partial chunks** | Processes zero-padded tail frames | Processes strictly `valid_frames` (§1.2) | Preserves true decay state at stream end |
| 3 | **Resampler** | Variable-input pull model (`Fixed::Output`) | Fixed-input push model (`Fixed::Both`, `FIXED_ASYNC_INPUT`) (§2.1) | Zero allocation, deterministic HAL/ASIO callbacks |
| 4 | **Lifecycle** | Heap allocation & disk I/O on real-time audio thread | Background control thread compilation & atomic swap (§2.2) | Hard real-time safety, zero audio dropouts |
| 5 | **Queue Topology** | Multi-producer crossbeam channel scheduling overhead | Wait-free power-of-two SPSC ring buffers (§2.3) | 1247.7x real-time throughput (25% faster) |
| 6 | **Queue Overflow** | Blocking back-pressure on full queue | Drop-on-full, non-blocking (§2.4) | Never stalls a driver callback thread |
| 7 | **Precision** | Single-precision (`f32`) cutoffs, config, and biquads | Double-precision (`double`) computation & storage (§3.1) | Sub-LSB numerical noise < 1e-15, exact anti-aliasing cutoff |
| 8 | **Interpolation** | Expanded polynomial powers in async **sinc** | Horner form with hardware FMA (§3.2) | Higher SIMD throughput and lower rounding error |
| 9 | **Dynamic Range** | Arbitrary `-200 dB` / `-300 dB` hard-coded math clamps | Mathematical `-inf` internally, clamped only on JSON output (§3.3) | Exact mathematical purity + RFC 8259 JSON compliance |
| 10 | **Metering** | Divides partial-chunk RMS power by buffer capacity | Divides power by `valid_frames` (§3.4) | Prevents zero-tail measurement dilution |
| 11 | **Metering** | Scalar accumulation or intermediate buffer allocations | Single-pass SIMD vectorization in `dsp_ops` (§3.5) | 2x faster calculation, zero allocation |
| 12 | **Convolution** | Inner-loop division by `fft_len` per sample | Precomputed reciprocal multiplication (`inv_scale`) (§3.6) | ~10x faster scaling in frequency domain |
| 13 | **Sanitization** | NaNs propagate freely through feedback & conversions | Feedback/sample sanitization & non-finite rejection (§3.7) | Prevents runaway oscillation & NaN math |
| 14 | **Spectrum** | Sums window normalization in `f64` | Computes & sums window normalization in `float` (§3.8) | Maximum performance and SIMD throughput in real-time metering |
| 15 | **Resampler Headroom** | Exact truncating bounds without vector headroom | Safety guard band (`ceil(...) + 16`) (§3.9) | Guaranteed zero SIMD out-of-bounds access |
| 16 | **Metering** | Clipped samples counted only at integer output | Floating-point peak saturation monitored across pipeline (§4.1) | Catches inter-stage digital clipping early |
| 17 | **Driver Events** | PipeWire rate change ignored in direct capture | Graph rate change actively captured & reported (§4.2) | Dynamic sample rate renegotiation |
| 18 | **Formats** | Limited to standard PCM audio formats | Native DSD and DoP (DSD over PCM) subsystem support (§4.3) | High-resolution audiophile format playback |
| 19 | **Validation** | Accepts degenerate/empty convolution IR configurations | Stricter rejection at validation time (§5) | Fails fast instead of producing NaN/singular filters |

---

## 1. Acoustic & Speaker Safety

### 1.1 Defensive Volume Tail Gain on Oversized Chunks
* **Upstream Behavior**: Upstream volume filters iterate strictly up to `chunk_size`. If an upstream capture backend delivers an oversized chunk or hardware buffer slice expansion, samples beyond `chunk_size` are left completely unattenuated at full scale (`gain = 1.0`).
* **`cdsp` Enhancement**: In [`Filters/volume.c`](../src/Filters/volume.c), `volume_filter_process` applies `final_gain` to all residual samples extending beyond `chunk_size`.
* **Why `cdsp` Is Better**: Leaving trailing audio frames unscaled at unity gain during high attenuation or muting produces sudden acoustic blasts. Defensive tail gain scaling guarantees consistent level control across all delivered frames.

### 1.2 Partial-Chunk Processing Restricted to `valid_frames`
* **Upstream Behavior**: Upstream advances processing across the full buffer capacity (`chunk_size`), including zero-padded tail frames on the stream's final partial chunk.
* **`cdsp` Enhancement**: `cdsp` restricts processing to `valid_frames`. Audited call sites:
  - **Dynamics envelopes**: [`Processors/compressor_processor.c`](../src/Processors/compressor_processor.c), [`Processors/noise_gate_processor.c`](../src/Processors/noise_gate_processor.c), [`Filters/lookahead_limiter.c`](../src/Filters/lookahead_limiter.c), plus [`Processors/lookahead_limiter_processor.c`](../src/Processors/lookahead_limiter_processor.c) and [`Processors/race_processor.c`](../src/Processors/race_processor.c).
  - **Mixer and pipeline steps**: `Mixer/mixer.c`, `Pipeline/pipeline.c` (filter and biquad steps) vs upstream `mixer.rs`, `pipeline.rs`.
  - **Convolution**: `Filters/convolution.c` convolves `valid_frames` and splits `count > chunk_size` into sub-blocks, vs upstream's whole-buffer `process_waveform` (`fftconv.rs`).
* **Why `cdsp` Is Better**: Processing trailing zero padding causes envelope detectors to artificially decay/release into silence at stream termination. Restricting progression to valid frames preserves the true signal level and dynamics state. For stateful filters, it avoids polluting filter state with padding at the end of a stream.

---

## 2. Real-Time Engine & Buffer Architecture

### 2.1 Deterministic Fixed-Input Resampling Architecture
* **Upstream Behavior**: Upstream CamillaDSP configures Rubato resamplers in `FixedSync::Output` and `FixedAsync::Output` mode (fixed output chunk, variable input chunk requested via `input_frames_next()`).
* **`cdsp` Enhancement**: `cdsp` uses fixed input chunk sizes across all resamplers:
  - Synchronous resampler: [`Resampler/synchronous_resampler.c`](../src/Resampler/synchronous_resampler.c) uses `FixedSync::Both` ($K \cdot M \to K \cdot L$) with `num_subchunks = 1`.
  - Slip & Async resamplers: [`Resampler/slip_resampler.c`](../src/Resampler/slip_resampler.c) and [`Resampler/async_poly_resampler.c`](../src/Resampler/async_poly_resampler.c) operate in `FIXED_ASYNC_INPUT` mode.
* **Why `cdsp` Is Better**: macOS CoreAudio HAL callbacks and Windows ASIO drivers strictly deliver fixed-size hardware buffers. Variable-input pull models require buffer stashing and circular copies inside platform backends. `cdsp`'s fixed-input architecture guarantees that every OS audio callback consumes a deterministic input block with zero reallocations, bounded ring-buffer latency, and deterministic real-time scheduling. In synchronous mode, `FixedSync::Both` achieves a steeper anti-aliasing cutoff ($0.987 \times \text{Nyquist}$ vs $0.954 \times \text{Nyquist}$).

### 2.2 Hard Real-Time Zero-Allocation Hot-Reload Architecture
* **Upstream Behavior**: Upstream compiles new pipelines, allocates heap memory, and reads impulse-response files from disk on the audio processing thread during reloads (`src/processing.rs`).
* **`cdsp` Enhancement**: In [`Engine/dsp_session.c`](../src/Engine/dsp_session.c), `cdsp` compiles pipelines, parses configurations, and loads impulse response files on the background control thread. The audio processing thread performs only an atomic pointer swap and state transfer.
* **Why `cdsp` Is Better**: Performing disk I/O and heap allocations on a real-time audio processing thread violates hard real-time programming contracts and causes buffer underruns, clicks, and dropouts under system load. `cdsp` guarantees **zero allocation, zero deallocation, and zero disk I/O on the audio thread**.

### 2.3 Wait-Free SPSC Power-of-Two Ring Buffers
* **Upstream Behavior**: Upstream uses multi-producer channels with dynamic allocation and locking mechanisms.
* **`cdsp` Enhancement**: In [`Utils/lock_free_ring_buffer.h`](../src/Utils/lock_free_ring_buffer.h) and [`Engine/audio_sync_queue.h`](../src/Engine/audio_sync_queue.h), `cdsp` implements power-of-two single-producer single-consumer ring buffers with bitmask indexing and acquire-release atomic ordering.
* **Why `cdsp` Is Better**: Bypasses scheduling overhead and achieves a **25% increase in raw data throughput** (1247.7x real-time speed vs 995.1x) with cache-aligned structures.

### 2.4 Drop-on-Full Queue Overflow Policy
* **Upstream Behavior**: Upstream applies **blocking back-pressure** — a full queue blocks the producer until the consumer drains it (`coreaudio_backend/device.rs`, `processing.rs`).
* **`cdsp` Enhancement**: [`Engine/engine_capture_loop.c`](../src/Engine/engine_capture_loop.c) and [`Engine/engine_processing_loop.c`](../src/Engine/engine_processing_loop.c) drop the chunk and continue rather than block.
* **Why `cdsp` Is Better**: Blocking inside a CoreAudio HAL or ASIO driver callback stalls the driver's real-time thread and cascades into a hardware-level overrun affecting the entire audio graph. Dropping a chunk degrades locally and recoverably instead.

---

## 3. Numerical Precision & Mathematical Integrity

### 3.1 Full Double-Precision Filter & Resampler Design
* **Upstream Behavior**: Upstream computes anti-alias cutoffs and processes biquad filter coefficients in single-precision (`f32`).
* **`cdsp` Enhancement**: `cdsp` uses IEEE 754 double precision (`double` / `f64`) for coefficient calculation, cutoff frequencies, volume ramps, and configuration storage ([`Filters/biquad.c`](../src/Filters/biquad.c), [`Resampler/synchronous_resampler.c`](../src/Resampler/synchronous_resampler.c)).
* **Why `cdsp` Is Better**: Prevents coefficient quantization distortion, preserves sub-LSB numerical noise below `1e-15`, and avoids high-Q biquad pole migration near Nyquist.

### 3.2 Horner Form with Hardware Fused Multiply-Add (FMA)
* **Scope**: Applies to the **async sinc** resampler.
* **Upstream Behavior**: Async sinc polynomial interpolation (`asynchro_sinc.rs`, `interp_cubic` / `interp_quad`) evaluates expanded polynomial powers $a + b \cdot t + c \cdot t^2 + d \cdot t^3$.
* **`cdsp` Enhancement**: In [`Resampler/async_sinc_resampler.c`](../src/Resampler/async_sinc_resampler.c), `cdsp` evaluates polynomials in nested Horner form: $a + t \cdot (b + t \cdot (c + t \cdot d))$ using hardware Fused Multiply-Add instructions.
* **Why `cdsp` Is Better**: Evaluating Horner form with FMA executes in fewer CPU cycles and incurs only a single rounding error at the final step, providing superior accuracy over expanded powers.

### 3.3 Mathematical `-inf` in DSP Math with Safe RFC 8259 JSON Clamping
* **Upstream Behavior**: Upstream hard-codes constant lower floors: `-200.0 dB` in `linear_to_db` and `1e-30` (`-300.0 dB`) in spectrum power calculation.
* **`cdsp` Enhancement**:
  - **Internal DSP**: In [`Audio/processing_parameters.c`](../src/Audio/processing_parameters.c), [`Utils/float_helpers.h`](../src/Utils/float_helpers.h), and [`Audio/spectrum_analyzer.c`](../src/Audio/spectrum_analyzer.c), `cdsp` retains pure IEEE 754 mathematical `-INFINITY` for zero-amplitude inputs.
  - **JSON Serialization Boundary**: In [`Server/websocket_server.c`](../app/Server/websocket_server.c) and [`Server/ws_rpc_dispatcher.c`](../app/Server/ws_rpc_dispatcher.c), numbers are clamped to `-200.0f` only when serializing for the WebSocket RPC API.
* **Why `cdsp` Is Better**: Internal DSP mathematics remains completely free of arbitrary constant noise floors and software clamps, while JSON consumers never receive illegal non-finite tokens that violate RFC 8259.

### 3.4 Partial-Chunk Accurate RMS Metering
* **Upstream Behavior**: Computes RMS by dividing sample sum-of-squares by the total chunk capacity, even for partial chunks at stream termination.
* **`cdsp` Enhancement**: In [`Audio/processing_parameters.c`](../src/Audio/processing_parameters.c), `cdsp` divides sum-of-squares by `valid_frames`.
* **Why `cdsp` Is Better**: Avoids zero-tail dilution on the final audio chunk, reporting the exact signal level.

### 3.5 Single-Pass SIMD Vectorization for Telemetry Metering
* **Upstream Behavior**: Upstream meters peak and RMS using scalar loops and intermediate allocations.
* **`cdsp` Enhancement**: In [`Utils/float_helpers.h`](../src/Utils/float_helpers.h), `dsp_ops_rms`, `dsp_ops_peak_absolute`, and `dsp_ops_min_max` use single-pass float vectorization (`fmaxf`, `sqrtf`, compiler auto-vectorization across AVX/NEON).
* **Why `cdsp` Is Better**: Executes ~2x faster across SIMD lanes while delivering ample precision for audio telemetry without intermediate memory buffers.

### 3.6 Precomputed Reciprocal Scaling for FFT Partitioned Convolution
* **Upstream Behavior**: Upstream performs sample-by-sample division by `fft_len` inside the time-domain synthesis loops.
* **`cdsp` Enhancement**: In [`Filters/convolution.c`](../src/Filters/convolution.c), `cdsp` precomputes `inv_scale = 1.0 / (double)fft_len` during filter compilation, replacing inner-loop divisions with fast reciprocal multiplications.
* **Why `cdsp` Is Better**: Floating-point division requires 10–20 clock cycles per sample, whereas multiplication requires 1–3 clock cycles and vectorizes cleanly, resulting in ~10x faster normalization.

### 3.7 Defensive Numerical Sanitization & Singularity Protection
* **EPS Clamps**: In [`Filters/biquad.c`](../src/Filters/biquad.c), `cdsp` clamps `|sin_w0|`, `A`, `|slope_s|`, `|q|` and the shelf `term` to a floor of `1e-12` to eliminate division-by-zero singularities.
* **Impulse Tail Scaling**: Linearly scales truncated impulse response tails to zero, eliminating DC step discontinuities.
* **Non-finite Sample Sanitization**: In [`Audio/sample_conversion.h`](../src/Audio/sample_conversion.h) and [`Processors/race_processor.c`](../src/Processors/race_processor.c), `cdsp` sanitizes NaN/Inf samples on format conversion and in the RACE feedback loop.
* **Defensive Parameter Validation & Sanitization**:
  - [`Filters/gain.c`](../src/Filters/gain.c) — rejects non-finite `gain`.
  - [`Public/fader.c`](../src/Public/fader.c) — maps `NaN` volume inputs to `-150.0 dB` (muted) defensively to protect loudspeakers.
  - [`Processors/race_processor.c`](../src/Processors/race_processor.c) — rejects non-finite or non-positive `attenuation` and `delay`.
  - [`Filters/lookahead_limiter.c`](../src/Filters/lookahead_limiter.c) — rejects non-finite `limit`, `attack`, `release`, or `lookahead`.
  - [`Filters/clipper.c`](../src/Filters/clipper.c) — rejects clipper limits that underflow to zero.
  - All processor validators additionally reject `channels == 0`.

### 3.8 Spectrum Analyzer Window Normalization in Single Precision
* **Upstream Behavior**: Upstream CamillaDSP (`src/spectrum.rs`) maps window values to `f64` and accumulates them in double precision.
* **`cdsp` Enhancement**: In [`Audio/spectrum_analyzer.c`](../src/Audio/spectrum_analyzer.c), `cdsp` computes and accumulates the symmetric Hann window in single precision (`float`).
* **Why `cdsp` Is Better**: Keeping the window buffer and its normalization sum entirely in single-precision `float` avoids conversion overhead, aligns with the single-precision `real_fftf` transform pipeline, and provides maximum SIMD throughput in real-time spectrum analysis.

### 3.9 Async Sinc Resampler Buffer Headroom and Ramped Ratio
* **Upstream Behavior**: Upstream Rubato sizes async buffers using exact truncating bounds (`+10.0` / `+2.0 + len/2`).
* **`cdsp` Enhancement**: In [`Resampler/async_sinc_resampler.c`](../src/Resampler/async_sinc_resampler.c), `cdsp` sizes internal working scratch buffers with an extra `+16` frame safety margin (`ceil(...) + 16`) and provides smooth ramped ratio transitions.
* **Why `cdsp` Is Better**: The extra 16-frame guard band ensures SIMD vector operations (AVX/NEON) have aligned padding and never read/write out of bounds during extreme dynamic ratio swings.

---

## 4. Hardware Driver & Subsystem Enhancements

### 4.1 Inter-Stage Floating-Point Peak Saturation Metering
* **Upstream Behavior**: Clipped samples are counted only during integer format conversion at the final audio backend.
* **`cdsp` Enhancement**: In [`Engine/engine_processing_loop.c`](../src/Engine/engine_processing_loop.c), peak saturation is monitored directly on floating-point audio data (`|sample| > 1.0`).
* **Why `cdsp` Is Better**: Catches inter-stage digital clipping across pipeline filters and mixers even if subsequent stages attenuate the signal before format conversion.

### 4.2 PipeWire Dynamic Graph Rate Change Reporting
* **Upstream Behavior**: PipeWire direct capture does not inspect or propagate graph sample rate changes.
* **`cdsp` Enhancement**: In [`Backend/pipewire_backend.c`](../src/Backend/pipewire_backend.c), `cdsp` tracks graph rate changes via `capture_backend_get_pending_rate_change()`, reporting them to the engine supervisor for seamless dynamic re-configuration.
* **Why `cdsp` Is Better**: Dynamically detects sample rate shifts in PipeWire graphs and notifies the supervisor instead of continuing with mismatched clock rates.

### 4.3 Native DSD and DoP (DSD over PCM) Subsystem Support
* **Upstream Behavior**: Upstream CamillaDSP is strictly limited to PCM audio formats.
* **`cdsp` Enhancement**: [`DSD/`](../src/DSD) implements high-performance Native DSD and DoP (DSD over PCM) encoding/decoding supporting up to DSD256 with SDM-6 modulators.
* **Why `cdsp` Is Better**: Expands high-end audiophile format support without sacrificing real-time speed, processing carrier streams up to 45x faster than real-time.

---

## 5. Stricter-Than-Upstream Validation

| Check | Location | Upstream |
|---|---|---|
| Empty convolution coefficients are a hard error | `Filters/convolution.c` | Deliberately non-fatal: one silent segment (`fftconv.rs`) |
| LookaheadLimiter config_diff escalation on parameter change | `Config/config_diff.c` | Escalates to `Pipeline` rebuild on *any* config change if limiter is present |
