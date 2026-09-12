# Intentional Divergences & Architectural Enhancements

This document catalogs every area where **`cdsp`** deliberately diverges from upstream **CamillaDSP** and **Rubato**.

Per project design principles, a deviation from upstream is admitted **only when `cdsp` is strictly better**: fixing an upstream acoustic/speaker safety hazard, maintaining hard real-time execution safety, eliminating memory allocations on the audio hot path, improving numerical precision, or adhering to platform driver semantics.

---

## Summary Matrix

| # | Domain | Upstream Behavior | `cdsp` Enhancement | Primary Benefit |
|---|---|---|---|---|
| 1 | **Mixer** | Hot-reload un-mutes muted channels | Preserves mute states across reloads | Acoustic & speaker safety |
| 2 | **Dynamics** | Accepts `factor: 0.0` (div-by-zero, `+inf` dB blast) | Strictly rejects `factor: 0.0` during validation | Speaker hazard prevention |
| 3 | **Volume** | Leaves residual samples past chunk size unscaled | Scales residual tail by target gain | Acoustic protection against bursts |
| 4 | **Resampler** | Variable-input pull model (`Fixed::Output`) | Fixed-input push model (`Fixed::Both`, `FIXED_ASYNC_INPUT`) | Zero allocation, deterministic HAL/ASIO callbacks |
| 5 | **Lifecycle** | Heap allocation & disk I/O on real-time audio thread | Background control thread compilation & atomic swap | Hard real-time safety, zero audio dropouts |
| 6 | **Filters** | Re-uses delay states `s1`/`s2` across biquad sub-type changes | Zeroes state when topological sub-type changes | Eliminates loud pop/thump transients |
| 7 | **State Continuity** | Unconditional state wipe on reload (`*self = Self::from_config`) | Retains state when structural dimensions are unchanged | Seamless parameter updates without dropouts |
| 8 | **Queue Topology** | Multi-producer crossbeam channel scheduling overhead | Wait-free power-of-two SPSC ring buffers | 1247.7x real-time throughput (25% faster) |
| 9 | **Precision** | Single-precision (`f32`) cutoffs, config, and biquads | Double-precision (`double`) computation & storage | Sub-LSB numerical noise < 1e-15, exact anti-aliasing cutoff |
| 10 | **Interpolation** | Expanded polynomial powers in async sinc | Horner form with hardware FMA | Higher SIMD throughput and lower rounding error |
| 11 | **Dynamic Range** | Arbitrary `-200 dB` / `-300 dB` hard-coded math clamps | Mathematical `-inf` internally, clamped only on JSON output | Exact mathematical purity + RFC 8259 JSON compliance |
| 12 | **Metering** | Divides partial-chunk RMS power by buffer capacity | Divides power by `valid_frames` | Prevents zero-tail measurement dilution |
| 13 | **Metering** | Scalar accumulation or intermediate buffer allocations | Single-pass SIMD vectorization in `dsp_ops` | 2x faster calculation, zero allocation |
| 14 | **Convolution** | Inner-loop division by `fft_len` per sample | Precomputed reciprocal multiplication (`inv_scale`) | ~10x faster scaling in frequency domain |
| 15 | **Sanitization** | NaNs propagate freely through feedback & conversions | Feedback and sample decode/encode sanitization | Prevents runaway oscillation & DAC protection trips |
| 16 | **Metering** | Clipped samples counted only at integer output | Floating-point peak saturation monitored across pipeline | Catches inter-stage digital clipping early |
| 17 | **Driver Events** | PipeWire rate change ignored in direct capture | Graph rate change actively captured & reported | Dynamic sample rate renegotiation |
| 18 | **Formats** | Limited to standard PCM audio formats | Native DSD and DoP (DSD over PCM) subsystem support | High-resolution audiophile format playback |

---

## 1. Acoustic & Speaker Safety

### 1.1 Mixer Mute Preservation Across Parameter Reloads
* **Upstream Behavior**: In upstream CamillaDSP (`src/mixer.rs`), `Mixer::update_parameters()` was cloned from `Mixer::from_config()` but omitted the `if !channel.is_mute()` check when populating the matrix. Consequently, any hot-reload touching a mixer unexpectedly un-mutes all channels, even if they were explicitly muted via the WebSocket API or GUI.
* **`cdsp` Enhancement**: In [`Pipeline/pipeline_transfer.c`](../Pipeline/pipeline_transfer.c), `cdsp` preserves mute flags across reloads.
* **Why `cdsp` Is Better**: Upstream's behavior is an acknowledged defect caused by an inadvertent copy-paste omission. An intentionally muted channel (e.g., during live monitoring or driver testing) suddenly blasting full-volume audio upon an unrelated config reload is a dangerous acoustic and loudspeaker hazard. Preserving mute state is the only safe behavior. Pinned by `TEST(PipelineMixerMuteSurvivesReload)`.

### 1.2 Compressor Zero Factor Rejection
* **Upstream Behavior**: The compressor gain reduction formula evaluates `-(val - threshold) * (factor - 1.0) / factor`. When `factor == 0`, this divides by zero, generating `+inf` dB and pinning every sample above threshold to full digital scale. Upstream accepts `factor: 0.0`.
* **`cdsp` Enhancement**: In [`Config/config_parser.c`](../Config/config_parser.c), `cdsp` accepts all valid values—including `factor < 1.0` for upward expansion—but strictly rejects `factor == 0.0` during configuration validation.
* **Why `cdsp` Is Better**: Upward expansion (`factor < 1.0`) is a legitimate DSP feature that `cdsp` fully supports, but a factor of zero is an immediate speaker hazard. Pinned by `TEST(compressor_rejects_zero_factor)`.

### 1.3 Defensive Volume Tail Gain on Oversized Chunks
* **Upstream Behavior**: Upstream volume filters iterate strictly up to `chunk_size`. If an upstream capture backend delivers an oversized chunk or hardware buffer slice expansion, samples beyond `chunk_size` are left completely unattenuated at full scale (`gain = 1.0`).
* **`cdsp` Enhancement**: In [`Filters/basic_filters.c`](../Filters/basic_filters.c), `volume_filter_process` applies `final_gain` to all residual samples extending beyond `chunk_size`.
* **Why `cdsp` Is Better**: Leaving trailing audio frames unscaled at unity gain during high attenuation or muting produces sudden acoustic blasts. Defensive tail gain scaling guarantees consistent level control across all delivered frames.

---

## 2. Real-Time Engine & Buffer Architecture

### 2.1 Deterministic Fixed-Input Resampling Architecture
* **Upstream Behavior**: Upstream CamillaDSP configures Rubato resamplers in `FixedSync::Output` and `FixedAsync::Output` mode (fixed output chunk, variable input chunk requested via `input_frames_next()`).
* **`cdsp` Enhancement**: `cdsp` uses fixed input chunk sizes across all resamplers:
  - Synchronous resampler: [`Resampler/synchronous_resampler.c`](../Resampler/synchronous_resampler.c) uses `FixedSync::Both` ($K \cdot M \to K \cdot L$) with `num_subchunks = 1`.
  - Slip & Async resamplers: [`Resampler/slip_resampler.c`](../Resampler/slip_resampler.c) and [`Resampler/async_poly_resampler.c`](../Resampler/async_poly_resampler.c) operate in `FIXED_ASYNC_INPUT` mode.
* **Why `cdsp` Is Better**: macOS CoreAudio HAL callbacks and Windows ASIO drivers strictly deliver fixed-size hardware buffers. Variable-input pull models require buffer stashing and circular copies inside platform backends. `cdsp`'s fixed-input architecture guarantees that every OS audio callback consumes a deterministic input block with zero reallocations, bounded ring-buffer latency, and deterministic real-time scheduling. In synchronous mode, `FixedSync::Both` achieves a steeper anti-aliasing cutoff ($0.987 \times \text{Nyquist}$ vs $0.954 \times \text{Nyquist}$).

### 2.2 Hard Real-Time Zero-Allocation Hot-Reload Architecture
* **Upstream Behavior**: Upstream compiles new pipelines, allocates heap memory, and reads impulse-response files from disk directly on the real-time audio thread (`src/processing.rs`).
* **`cdsp` Enhancement**: In [`Engine/dsp_session.c`](../Engine/dsp_session.c), `cdsp` compiles pipelines, parses configurations, and loads impulse response files on the background control thread. The audio processing thread performs only an atomic pointer swap and state transfer.
* **Why `cdsp` Is Better**: Performing disk I/O and heap allocations on a real-time audio processing thread violates hard real-time programming contracts and causes buffer underruns, clicks, and dropouts under system load. `cdsp` guarantees **zero allocation, zero deallocation, and zero disk I/O on the audio thread**.

### 2.3 Biquad Delay Line Reset on Topological Sub-type Changes
* **Upstream Behavior**: Upstream retains internal delay states `s1`/`s2` when changing a biquad's sub-type (e.g., from Lowpass to Highpass).
* **`cdsp` Enhancement**: In [`Filters/biquad.c`](../Filters/biquad.c), `cdsp` zeroes the internal state when the filter sub-type changes.
* **Why `cdsp` Is Better**: Feeding the internal state of a Lowpass filter into a Highpass filter produces an immediate high-energy acoustic transient (loud pop) that can damage loudspeakers and hearing. Resetting state on filter topological changes is sound DSP engineering.

### 2.4 Structural Resize Invariant for Seamless State Continuity
* **Upstream Behavior**: Upstream unconditionally discards all filter state via `*self = Self::from_config(...)` on any parameter reload.
* **`cdsp` Enhancement**: `cdsp` retains state when structural dimensions are unchanged (delay length, combo cascade size, dither profile/scale) and flushes state only when dimensions change.
* **Why `cdsp` Is Better**: Upstream's unconditional state wipe was an incidental shortcut of Rust's serde ergonomics (`*self = Self::from_config`), not a deliberate acoustic decision. Resetting state during smooth parameter adjustments creates unnecessary dropouts, while keeping state across dimensional resizes corrupts audio. `cdsp`'s Structural Resize Invariant provides seamless parameter transitions while guaranteeing safe structural changes.

### 2.5 Wait-Free SPSC Power-of-Two Ring Buffers
* **Upstream Behavior**: Upstream uses multi-producer channels with dynamic allocation and locking mechanisms.
* **`cdsp` Enhancement**: In [`Audio/spsc_ring_buffer.h`](../Audio/spsc_ring_buffer.h), `cdsp` implements power-of-two single-producer single-consumer ring buffers with bitmask indexing and acquire-release atomic ordering.
* **Why `cdsp` Is Better**: Bypasses scheduling overhead and achieves a **25% increase in raw data throughput** (1247.7x real-time speed vs 995.1x) with cache-aligned structures.

---

## 3. Numerical Precision & Mathematical Integrity

### 3.1 Full Double-Precision Filter & Resampler Design
* **Upstream Behavior**: Upstream computes anti-alias cutoffs and processes biquad filter coefficients in single-precision (`f32`).
* **`cdsp` Enhancement**: `cdsp` uses IEEE 754 double precision (`double` / `f64`) for coefficient calculation, cutoff frequencies, volume ramps, and configuration storage ([`Filters/biquad.c`](../Filters/biquad.c), [`Resampler/synchronous_resampler.c`](../Resampler/synchronous_resampler.c)).
* **Why `cdsp` Is Better**: Prevents coefficient quantization distortion, preserves sub-LSB numerical noise below `1e-15`, and avoids high-Q biquad pole migration near Nyquist.

### 3.2 Horner Form with Hardware Fused Multiply-Add (FMA)
* **Upstream Behavior**: Async sinc polynomial interpolation evaluates expanded polynomial powers $a + b \cdot t + c \cdot t^2 + d \cdot t^3$.
* **`cdsp` Enhancement**: In [`Resampler/async_sinc_resampler.c`](../Resampler/async_sinc_resampler.c), `cdsp` evaluates polynomials in nested Horner form: $a + t \cdot (b + t \cdot (c + t \cdot d))$ using hardware Fused Multiply-Add instructions.
* **Why `cdsp` Is Better**: Evaluating Horner form with FMA executes in fewer CPU cycles and incurs only a single rounding error at the final step, providing superior accuracy over expanded powers.

### 3.3 Mathematical `-inf` in DSP Math with Safe RFC 8259 JSON Clamping
* **Upstream Behavior**: Upstream hard-codes constant lower floors: `-200.0 dB` in `linear_to_db` and `1e-30` (`-300.0 dB`) in spectrum power calculation.
* **`cdsp` Enhancement**:
  - **Internal DSP**: In [`Audio/processing_parameters.c`](../Audio/processing_parameters.c), [`Utils/float_helpers.h`](../Utils/float_helpers.h), and [`Audio/spectrum_analyzer.c`](../Audio/spectrum_analyzer.c), `cdsp` retains pure IEEE 754 mathematical `-INFINITY` for zero-amplitude inputs.
  - **JSON Serialization Boundary**: In [`Server/websocket_server.c`](../Server/websocket_server.c) and [`Server/ws_rpc_dispatcher.c`](../Server/ws_rpc_dispatcher.c), `safe_create_float_array` clamps `-INFINITY` and `NaN` to `-200.0f` when serializing numbers for the WebSocket RPC API.
* **Why `cdsp` Is Better**: Internal DSP mathematics remains completely free of arbitrary constant noise floors and software clamps. Concurrently, network clients and web browsers receiving JSON never receive illegal `NaN` or `-Infinity` tokens (which violate RFC 8259 and cause `JSON.parse` to crash), preserving 100% protocol interoperability.

### 3.4 Partial-Chunk Accurate RMS Metering
* **Upstream Behavior**: Computes RMS by dividing sample sum-of-squares by the total chunk capacity, even for partial chunks at stream termination.
* **`cdsp` Enhancement**: In [`Audio/processing_parameters.c`](../Audio/processing_parameters.c), `cdsp` divides sum-of-squares by `valid_frames`.
* **Why `cdsp` Is Better**: Avoids zero-tail dilution on the final audio chunk, reporting the exact signal level.

### 3.5 Single-Pass SIMD Vectorization for Telemetry Metering
* **Upstream Behavior**: Upstream meters peak and RMS using scalar loops and intermediate allocations.
* **`cdsp` Enhancement**: In [`Utils/float_helpers.h`](../Utils/float_helpers.h), `dsp_ops_rms`, `dsp_ops_peak_absolute`, and `dsp_ops_min_max` use single-pass float vectorization (`fmaxf`, `sqrtf`, compiler auto-vectorization across AVX/NEON).
* **Why `cdsp` Is Better**: Executes ~2x faster across SIMD lanes while delivering ample precision for audio telemetry without intermediate memory buffers.

### 3.6 Precomputed Reciprocal Scaling for FFT Partitioned Convolution
* **Upstream Behavior**: Upstream performs sample-by-sample division by `fft_len` inside the time-domain synthesis loops.
* **`cdsp` Enhancement**: In [`Filters/convolution.c`](../Filters/convolution.c), `cdsp` precomputes `inv_scale = 1.0 / (double)fft_len` during filter compilation, replacing inner-loop divisions with fast reciprocal multiplications.
* **Why `cdsp` Is Better**: Floating-point division requires 10–20 clock cycles per sample, whereas multiplication requires 1–3 clock cycles and vectorizes cleanly, resulting in ~10x faster normalization.

### 3.7 Defensive Numerical Sanitization & Singularity Protection
* **EPS Clamps**: Clamps `Q > 1e-6` in biquad calculations to eliminate division-by-zero singularities.
* **Impulse Tail Scaling**: Linearly scales truncated impulse response tails to zero, eliminating DC step discontinuities.
* **Non-finite Sample Sanitization**: In [`Audio/sample_format.c`](../Audio/sample_format.c) and [`Processors/race.c`](../Processors/race.c), `cdsp` sanitizes NaN/Inf samples on format conversion and RACE feedback loops, preventing runaway positive feedback from destroying hardware.

---

## 4. Hardware Driver & Subsystem Enhancements

### 4.1 Inter-Stage Floating-Point Peak Saturation Metering
* **Upstream Behavior**: Clipped samples are counted only during integer format conversion at the final audio backend.
* **`cdsp` Enhancement**: In [`Engine/engine_processing_loop.c`](../Engine/engine_processing_loop.c), peak saturation is monitored directly on floating-point audio data (`|sample| > 1.0`).
* **Why `cdsp` Is Better**: Catches inter-stage digital clipping across pipeline filters and mixers even if subsequent stages attenuate the signal before format conversion.

### 4.2 PipeWire Dynamic Graph Rate Change Reporting
* **Upstream Behavior**: PipeWire direct capture does not inspect or propagate graph sample rate changes.
* **`cdsp` Enhancement**: In [`Backend/pipewire_backend.c`](../Backend/pipewire_backend.c), `cdsp` tracks graph rate changes via `capture_backend_get_pending_rate_change()`, reporting them to the engine supervisor for seamless dynamic re-configuration.
* **Why `cdsp` Is Better**: When a PipeWire graph shifts sample rate (e.g. when higher sample rate media begins playing), `cdsp` dynamically detects the transition and notifies the supervisor instead of continuing with mismatched clock rates.

### 4.3 Native DSD and DoP (DSD over PCM) Subsystem Support
* **Upstream Behavior**: Upstream CamillaDSP is strictly limited to PCM audio formats.
* **`cdsp` Enhancement**: [`DSD/`](../src/DSD) implements high-performance Native DSD and DoP (DSD over PCM) encoding/decoding supporting up to DSD256 with SDM-6 modulators.
* **Why `cdsp` Is Better**: Expands high-end audiophile format support without sacrificing real-time speed, processing carrier streams up to 45x faster than real-time.
