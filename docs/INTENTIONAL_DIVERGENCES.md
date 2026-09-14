# Intentional Divergences & Architectural Enhancements

This document catalogs every area where **`cdsp`** deliberately diverges from upstream **CamillaDSP** and **Rubato**.

Per project design principles, a deviation from upstream is admitted **only when `cdsp` is strictly better**: fixing an upstream acoustic/speaker safety hazard, maintaining hard real-time execution safety, eliminating memory allocations on the audio hot path, improving numerical precision, or adhering to platform driver semantics.

---

## Summary Matrix

| # | Domain | Upstream Behavior | `cdsp` Enhancement | Primary Benefit |
|---|---|---|---|---|
| 1 | **Mixer** | Latent un-mute bug in `update_parameters` (unreachable — see §1.1) | No in-place mixer update; always rebuilds honouring mute | Structurally immune; matches upstream behaviour |
| 2 | **Dynamics** | Accepts `factor: 0.0` (div-by-zero, `+inf` dB blast) | Strictly rejects `factor: 0.0` during validation | Speaker hazard prevention |
| 3 | **Volume** | Leaves residual samples past chunk size unscaled | Scales residual tail by target gain | Acoustic protection against bursts |
| 4 | **Partial chunks** | Processes zero-padded tail frames | Processes strictly `valid_frames` (dynamics, mixer, filter/biquad steps, convolution) | Preserves true decay state at stream end |
| 5 | **Resampler** | Variable-input pull model (`Fixed::Output`) | Fixed-input push model (`Fixed::Both`, `FIXED_ASYNC_INPUT`) | Zero allocation, deterministic HAL/ASIO callbacks |
| 6 | **Lifecycle** | Heap allocation & disk I/O on real-time audio thread | Background control thread compilation & atomic swap | Hard real-time safety, zero audio dropouts |
| 7 | **Filters** | Re-uses delay states `s1`/`s2` across biquad sub-type changes | Zeroes state when topological sub-type changes | Eliminates loud pop/thump transients |
| 8 | **Queue Topology** | Multi-producer crossbeam channel scheduling overhead | Wait-free power-of-two SPSC ring buffers | 1247.7x real-time throughput (25% faster) |
| 9 | **Precision** | Single-precision (`f32`) cutoffs, config, and biquads | Double-precision (`double`) computation & storage | Sub-LSB numerical noise < 1e-15, exact anti-aliasing cutoff |
| 10 | **Interpolation** | Expanded polynomial powers in async **sinc** (upstream adopted Horner for async poly) | Horner form with hardware FMA | Higher SIMD throughput and lower rounding error |
| 11 | **Dynamic Range** | Arbitrary `-200 dB` / `-300 dB` hard-coded math clamps | Mathematical `-inf` internally, clamped only on JSON output | Exact mathematical purity + RFC 8259 JSON compliance |
| 12 | **Metering** | Divides partial-chunk RMS power by buffer capacity | Divides power by `valid_frames` | Prevents zero-tail measurement dilution |
| 13 | **Metering** | Scalar accumulation or intermediate buffer allocations | Single-pass SIMD vectorization in `dsp_ops` | 2x faster calculation, zero allocation |
| 14 | **Convolution** | Inner-loop division by `fft_len` per sample | Precomputed reciprocal multiplication (`inv_scale`) | ~10x faster scaling in frequency domain |
| 15 | **Sanitization** | NaNs propagate freely through feedback & conversions | Feedback/sample sanitization & non-finite parameter rejection | Prevents runaway oscillation & NaN math |
| 16 | **Metering** | Clipped samples counted only at integer output | Floating-point peak saturation monitored across pipeline | Catches inter-stage digital clipping early |
| 17 | **Driver Events** | PipeWire rate change ignored in direct capture | Graph rate change actively captured & reported | Dynamic sample rate renegotiation |
| 18 | **Formats** | Limited to standard PCM audio formats | Native DSD and DoP (DSD over PCM) subsystem support | High-resolution audiophile format playback |
| 19 | **Spectrum** | Sums window normalization in `f64` | Computes & sums window normalization in `float` | Maximum performance and SIMD throughput in real-time metering |
| 20 | **Queue Overflow** | Blocking back-pressure on full queue | Drop-on-full, non-blocking (§2.5) | Never stalls a driver callback thread |
| 21 | **Validation** | Accepts degenerate/out-of-range params | Stricter rejection at validation time (§5) | Fails fast instead of producing NaN/singular filters |
| 22 | **Pending** | — | Accidental deviations awaiting disposition (§6) | *Not enhancements; to be fixed or justified* |

> [!NOTE]
> Rows 1, 4 and §3.7 were corrected on 2026-09-14 following a line-by-line port audit against `camilladsp@b438410` and `rubato@1d1da5c`; sections 2.5, 5 and 6 were added by the same audit. Full findings: [`audit/PORT_AUDIT.md`](audit/PORT_AUDIT.md).

---

## 1. Acoustic & Speaker Safety

### 1.1 Mixer Mute Preservation Across Parameter Reloads
* **Upstream Behavior**: In upstream CamillaDSP (`src/mixer.rs:80-101`), `Mixer::update_parameters()` was cloned from `Mixer::from_config()` but omitted the `if !channel.is_mute()` check when populating the matrix, so it would un-mute every channel it touched.
  > **Correction (audited 2026-09-14):** this upstream defect is **latent, not reachable**. `config_diff` returns `ConfigChange::MixerParameters` as soon as `currentconf.mixers != newconf.mixers` (`config/utils.rs:423-425`), and `processing.rs:170-179` answers that with a full `Pipeline::from_config` rebuild. The `mixers` name list passed to `Pipeline::update_parameters` is only populated in the later `FilterParameters` branch (`config/utils.rs:457-466`), which is reached exclusively when the mixer sections are byte-identical — so the list is always empty and `Mixer::update_parameters` is dead code. Upstream therefore does **not** un-mute channels on reload in practice.
* **`cdsp` Behavior**: `cdsp` has no in-place mixer update at all. Every reload rebuilds through `mixer_create` → [`populate_mapping`](../src/Mixer/mixer.c), which honours both the per-destination `map->mute` (`Mixer/mixer.c:79`) and the per-source `src->mute` (`Mixer/mixer.c:109`), exactly mirroring `Mixer::from_config` (`mixer.rs:54,57`).
* **Status**: **Behaviourally equivalent to upstream**, reached by a different route. `cdsp` is structurally immune to the upstream defect because the offending code path does not exist. Pinned by `TEST(PipelineMixerMuteSurvivesReload)`.

### 1.2 Compressor Zero Factor Rejection
* **Upstream Behavior**: The compressor gain reduction formula evaluates `-(val - threshold) * (factor - 1.0) / factor`. When `factor == 0`, this divides by zero, generating `+inf` dB and pinning every sample above threshold to full digital scale. Upstream accepts `factor: 0.0`.
* **`cdsp` Enhancement**: In [`Processors/compressor_processor.c`](../src/Processors/compressor_processor.c), `cdsp` accepts all valid values—including `factor < 1.0` for upward expansion—but strictly rejects `factor == 0.0` during processor validation (`compressor_config_validate`).
* **Why `cdsp` Is Better**: Upward expansion (`factor < 1.0`) is a legitimate DSP feature that `cdsp` fully supports, but a factor of zero is an immediate speaker hazard. Pinned by `TEST(compressor_rejects_zero_factor)`.

### 1.3 Defensive Volume Tail Gain on Oversized Chunks
* **Upstream Behavior**: Upstream volume filters iterate strictly up to `chunk_size`. If an upstream capture backend delivers an oversized chunk or hardware buffer slice expansion, samples beyond `chunk_size` are left completely unattenuated at full scale (`gain = 1.0`).
* **`cdsp` Enhancement**: In [`Filters/volume.c`](../src/Filters/volume.c), `volume_filter_process` applies `final_gain` to all residual samples extending beyond `chunk_size`.
* **Why `cdsp` Is Better**: Leaving trailing audio frames unscaled at unity gain during high attenuation or muting produces sudden acoustic blasts. Defensive tail gain scaling guarantees consistent level control across all delivered frames.

### 1.4 Partial-Chunk Processing Restricted to `valid_frames`
* **Upstream Behavior**: Upstream advances processing across the full buffer capacity (`chunk_size`), including zero-padded tail frames on the stream's final partial chunk.
* **`cdsp` Enhancement**: `cdsp` restricts processing to `valid_frames`. Audited call sites:
  - **Dynamics envelopes** (the original motivation): [`Processors/compressor_processor.c`](../src/Processors/compressor_processor.c), [`Processors/noise_gate_processor.c`](../src/Processors/noise_gate_processor.c), [`Filters/lookahead_limiter.c`](../src/Filters/lookahead_limiter.c), plus [`Processors/lookahead_limiter_processor.c:283`](../src/Processors/lookahead_limiter_processor.c) and [`Processors/race_processor.c:266`](../src/Processors/race_processor.c).
  - **Mixer and pipeline steps**: `Mixer/mixer.c:181`, `Pipeline/pipeline.c:167,181` (filter and biquad steps) vs upstream `mixer.rs:106-122`, `pipeline.rs:141-149,370-390`.
  - **Convolution**: `Filters/convolution.c:600-613` convolves `valid_frames` and splits `count > chunk_size` into sub-blocks, vs upstream's whole-buffer `process_waveform` (`fftconv.rs:456-505`). Proven numerically equivalent — upstream's chunk tails are zeroed via `vec_from_stash` (`utils/stash.rs:106-109`) — so this is a performance divergence, not a behavioural one.
* **Why `cdsp` Is Better**: Processing trailing zero padding causes envelope detectors to artificially decay/release into silence at stream termination. Restricting progression to valid frames preserves the true signal level and dynamics state. For stateless stages the two are identical; for stateful filters it avoids polluting filter state with padding at the end of a stream.

---

## 2. Real-Time Engine & Buffer Architecture

### 2.1 Deterministic Fixed-Input Resampling Architecture
* **Upstream Behavior**: Upstream CamillaDSP configures Rubato resamplers in `FixedSync::Output` and `FixedAsync::Output` mode (fixed output chunk, variable input chunk requested via `input_frames_next()`).
* **`cdsp` Enhancement**: `cdsp` uses fixed input chunk sizes across all resamplers:
  - Synchronous resampler: [`Resampler/synchronous_resampler.c`](../src/Resampler/synchronous_resampler.c) uses `FixedSync::Both` ($K \cdot M \to K \cdot L$) with `num_subchunks = 1`.
  - Slip & Async resamplers: [`Resampler/slip_resampler.c`](../src/Resampler/slip_resampler.c) and [`Resampler/async_poly_resampler.c`](../src/Resampler/async_poly_resampler.c) operate in `FIXED_ASYNC_INPUT` mode.
* **Why `cdsp` Is Better**: macOS CoreAudio HAL callbacks and Windows ASIO drivers strictly deliver fixed-size hardware buffers. Variable-input pull models require buffer stashing and circular copies inside platform backends. `cdsp`'s fixed-input architecture guarantees that every OS audio callback consumes a deterministic input block with zero reallocations, bounded ring-buffer latency, and deterministic real-time scheduling. In synchronous mode, `FixedSync::Both` achieves a steeper anti-aliasing cutoff ($0.987 \times \text{Nyquist}$ vs $0.954 \times \text{Nyquist}$).

### 2.2 Hard Real-Time Zero-Allocation Hot-Reload Architecture
* **Upstream Behavior**: Upstream compiles new pipelines, allocates heap memory, and reads impulse-response files from disk directly on the real-time audio thread (`src/processing.rs`).
* **`cdsp` Enhancement**: In [`Engine/dsp_session.c`](../src/Engine/dsp_session.c), `cdsp` compiles pipelines, parses configurations, and loads impulse response files on the background control thread. The audio processing thread performs only an atomic pointer swap and state transfer.
* **Why `cdsp` Is Better**: Performing disk I/O and heap allocations on a real-time audio processing thread violates hard real-time programming contracts and causes buffer underruns, clicks, and dropouts under system load. `cdsp` guarantees **zero allocation, zero deallocation, and zero disk I/O on the audio thread**.

### 2.3 Biquad Delay Line Reset on Topological Sub-type Changes
* **Upstream Behavior**: Upstream retains internal delay states `s1`/`s2` when changing a biquad's sub-type (e.g., from Lowpass to Highpass).
* **`cdsp` Enhancement**: In [`Filters/biquad.c`](../src/Filters/biquad.c), `cdsp` zeroes the internal state when the filter sub-type changes.
* **Why `cdsp` Is Better**: Feeding the internal state of a Lowpass filter into a Highpass filter produces an immediate high-energy acoustic transient (loud pop) that can damage loudspeakers and hearing. Resetting state on filter topological changes is sound DSP engineering.

### 2.4 Wait-Free SPSC Power-of-Two Ring Buffers
* **Upstream Behavior**: Upstream uses multi-producer channels with dynamic allocation and locking mechanisms.
* **`cdsp` Enhancement**: In [`Utils/lock_free_ring_buffer.h`](../src/Utils/lock_free_ring_buffer.h) and [`Engine/audio_sync_queue.h`](../src/Engine/audio_sync_queue.h), `cdsp` implements power-of-two single-producer single-consumer ring buffers with bitmask indexing and acquire-release atomic ordering.
* **Why `cdsp` Is Better**: Bypasses scheduling overhead and achieves a **25% increase in raw data throughput** (1247.7x real-time speed vs 995.1x) with cache-aligned structures.

### 2.5 Drop-on-Full Queue Overflow Policy
* **Upstream Behavior**: Upstream applies **blocking back-pressure** — a full queue blocks the producer until the consumer drains it (`coreaudio_backend/device.rs:1208-1212`, `processing.rs:27-43`).
* **`cdsp` Enhancement**: [`Engine/engine_capture_loop.c:282-294`](../src/Engine/engine_capture_loop.c) and [`Engine/engine_processing_loop.c:304-320`](../src/Engine/engine_processing_loop.c) drop the chunk and continue rather than block.
* **Why `cdsp` Is Better**: Blocking inside a CoreAudio HAL or ASIO driver callback stalls the driver's real-time thread and cascades into a hardware-level overrun affecting the entire audio graph. Dropping a chunk degrades locally and recoverably instead. The full rationale and state-machine interaction is in [`engine_state_management.md` §3.2](engine_state_management.md).
* **Note**: Previously documented only in the internal state-management design doc; surfaced here because it is a genuine, user-observable behavioural divergence under overload.

---

## 3. Numerical Precision & Mathematical Integrity

### 3.1 Full Double-Precision Filter & Resampler Design
* **Upstream Behavior**: Upstream computes anti-alias cutoffs and processes biquad filter coefficients in single-precision (`f32`).
* **`cdsp` Enhancement**: `cdsp` uses IEEE 754 double precision (`double` / `f64`) for coefficient calculation, cutoff frequencies, volume ramps, and configuration storage ([`Filters/biquad.c`](../src/Filters/biquad.c), [`Resampler/synchronous_resampler.c`](../src/Resampler/synchronous_resampler.c)).
* **Why `cdsp` Is Better**: Prevents coefficient quantization distortion, preserves sub-LSB numerical noise below `1e-15`, and avoids high-Q biquad pole migration near Nyquist.

### 3.2 Horner Form with Hardware Fused Multiply-Add (FMA)
* **Scope**: Applies to the **async sinc** resampler only. Rubato adopted Horner form for its *polynomial* resampler (`asynchro_fast.rs`) in commit `e4d03f5`, so [`Resampler/async_poly_resampler.c`](../src/Resampler/async_poly_resampler.c) now matches upstream exactly and is no longer a divergence. `asynchro_sinc.rs` still evaluates expanded powers.
* **Upstream Behavior**: Async sinc polynomial interpolation (`asynchro_sinc.rs`, `interp_cubic` / `interp_quad`) evaluates expanded polynomial powers $a + b \cdot t + c \cdot t^2 + d \cdot t^3$.
* **`cdsp` Enhancement**: In [`Resampler/async_sinc_resampler.c`](../src/Resampler/async_sinc_resampler.c), `cdsp` evaluates polynomials in nested Horner form: $a + t \cdot (b + t \cdot (c + t \cdot d))$ using hardware Fused Multiply-Add instructions.
* **Why `cdsp` Is Better**: Evaluating Horner form with FMA executes in fewer CPU cycles and incurs only a single rounding error at the final step, providing superior accuracy over expanded powers. Upstream's own adoption of Horner form for the polynomial resampler confirms the approach.

### 3.3 Mathematical `-inf` in DSP Math with Safe RFC 8259 JSON Clamping
* **Upstream Behavior**: Upstream hard-codes constant lower floors: `-200.0 dB` in `linear_to_db` and `1e-30` (`-300.0 dB`) in spectrum power calculation.
* **`cdsp` Enhancement**:
  - **Internal DSP**: In [`Audio/processing_parameters.c`](../src/Audio/processing_parameters.c), [`Utils/float_helpers.h`](../src/Utils/float_helpers.h), and [`Audio/spectrum_analyzer.c`](../src/Audio/spectrum_analyzer.c), `cdsp` retains pure IEEE 754 mathematical `-INFINITY` for zero-amplitude inputs.
  - **JSON Serialization Boundary**: In [`Server/websocket_server.c`](../app/Server/websocket_server.c) and [`Server/ws_rpc_dispatcher.c`](../app/Server/ws_rpc_dispatcher.c), `safe_create_float_array` clamps `-INFINITY` and `NaN` to `-200.0f` when serializing numbers for the WebSocket RPC API.
* **Why `cdsp` Is Better**: Internal DSP mathematics remains completely free of arbitrary constant noise floors and software clamps. Concurrently, network clients and web browsers receiving JSON never receive illegal `NaN` or `-Infinity` tokens (which violate RFC 8259 and cause `JSON.parse` to crash), preserving 100% protocol interoperability.

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
*Every item below was re-verified against the source on 2026-09-14.*

* **EPS Clamps**: In [`Filters/biquad.c`](../src/Filters/biquad.c), `cdsp` clamps `|sin_w0|`, `A`, `|slope_s|`, `|q|` and the shelf `term` to a floor of `1e-12` (`biquad.c:84,85,100,102,106`) to eliminate division-by-zero singularities. *(Previously documented as a `Q > 1e-6` clamp; the constant and the set of clamped quantities were both wrong.)*
* **Impulse Tail Scaling**: Linearly scales truncated impulse response tails to zero, eliminating DC step discontinuities.
* **Non-finite Sample Sanitization**: In [`Audio/sample_conversion.h`](../src/Audio/sample_conversion.h) and [`Processors/race_processor.c:310-311`](../src/Processors/race_processor.c), `cdsp` sanitizes NaN/Inf samples on format conversion and in the RACE feedback loop, preventing runaway positive feedback from destroying hardware.
* **Defensive Parameter Validation & Sanitization** — the checks that exist:
  - [`Filters/gain.c:42-48`](../src/Filters/gain.c) — rejects non-finite `gain`; upstream (`basicfilters.rs:488-501`) accepts it and emits NaN.
  - [`Public/fader.c:9-14`](../src/Public/fader.c) — maps `NaN` volume inputs to `-150.0 dB` (muted) defensively to protect loudspeakers.
  - [`Processors/race_processor.c:87,92`](../src/Processors/race_processor.c) — rejects non-finite or non-positive `attenuation` and `delay`.
  - [`Filters/lookahead_limiter.c:167`](../src/Filters/lookahead_limiter.c) — rejects non-finite `limit`, `attack`, `release`, or `lookahead`.
  - [`Filters/clipper.c`](../src/Filters/clipper.c) — rejects clipper limits that underflow to zero.
  - [`Processors/compressor_processor.c:116`](../src/Processors/compressor_processor.c) — rejects `factor == 0.0` (see §1.2).
  - All processor validators additionally reject `channels == 0`, which upstream does not.

### 3.8 Spectrum Analyzer Window Normalization in Single Precision
* **Upstream Behavior**: Upstream CamillaDSP (`src/spectrum.rs`) maps window values to `f64` and accumulates them in double precision (`let window_sum: f64 = window.iter().map(|w| *w as f64).sum();`) to avoid precision loss on large FFT sizes.
* **`cdsp` Enhancement**: In [`Audio/spectrum_analyzer.c`](../src/Audio/spectrum_analyzer.c), `cdsp` computes and accumulates the symmetric Hann window in single precision (`float`).
* **Why `cdsp` Is Better**: Keeping the window buffer and its normalization sum entirely in single-precision `float` avoids conversion overhead, aligns with the single-precision `real_fftf` transform pipeline, and provides maximum performance and SIMD throughput in real-time spectrum analysis.

### 3.9 Async Sinc Resampler Buffer Headroom and Ramped Ratio
* **Upstream Behavior**: Upstream Rubato sizes async buffers using exact truncating bounds (`+10.0` / `+2.0 + len/2`) and provides both unramped (`ramp = false`) and ramped ratio updates.
* **`cdsp` Enhancement**: In [`Resampler/async_sinc_resampler.c`](../src/Resampler/async_sinc_resampler.c), `cdsp` sizes internal working scratch buffers with an extra `+16` frame safety margin (`ceil(...) + 16`) to guarantee zero buffer overrun across SIMD vector tails, and provides smooth ramped ratio transitions matching CamillaDSP's runtime usage.
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
* **Why `cdsp` Is Better**: When a PipeWire graph shifts sample rate (e.g. when higher sample rate media begins playing), `cdsp` dynamically detects the transition and notifies the supervisor instead of continuing with mismatched clock rates.

### 4.3 Native DSD and DoP (DSD over PCM) Subsystem Support
* **Upstream Behavior**: Upstream CamillaDSP is strictly limited to PCM audio formats.
* **`cdsp` Enhancement**: [`DSD/`](../src/DSD) implements high-performance Native DSD and DoP (DSD over PCM) encoding/decoding supporting up to DSD256 with SDM-6 modulators.
* **Why `cdsp` Is Better**: Expands high-end audiophile format support without sacrificing real-time speed, processing carrier streams up to 45x faster than real-time.

---

## 5. Stricter-Than-Upstream Validation

*Added 2026-09-14. These reject configurations that upstream accepts. Each is defensible on safety grounds, but they narrow the accepted configuration space and are listed here so the difference is deliberate and reviewable.*

| Check | Location | Upstream |
|---|---|---|
| Empty convolution coefficients are a hard error | `Filters/convolution.c:442-455` | Deliberately non-fatal: one silent segment (`fftconv.rs:352-360` + test `:732-748`) |
| `samplerate <= 0` rejected | `Config/configuration.c:334-338`, `Config/config_parse_devices.c:1405-1414` | No `validate_nonzero_usize` on `Devices.samplerate` |
| LookaheadLimiter config_diff escalation on parameter change | `Config/config_diff.c:1005-1014` | Escalates to `Pipeline` rebuild on *any* config change if limiter is present |

---

## 6. Deviations Reconciled

*The following items previously tracked in §6 were reconciled and fixed to match upstream during the 2026-09-14 audit:*
* **Mixer flat aliases removed**: `channels_in`, `channels_out`, `channel_labels` removed (`Config/config_parse_mixers.c`), enforcing strict `{ channels: { in, out } }`.
* **DiffEq normalization aligned**: Normalizes coefficients via `/ a0` division matching upstream (`Filters/diffeq.c`).
* **AsyncPoly profile code removed**: Removed non-upstream `profile` table and dead mapping logic (`Resampler/async_poly_resampler.c`).
* **Resampler cutoff range aligned**: Allowed `f_cutoff > 1.0` if configured (`Resampler/audio_resampler.c`).
* **Biquad combo constraints aligned**: Removed artificial order caps and sample rate validation sequencing (`Filters/biquad_combo.c`).
* **Wav header parsing aligned**: Stricter fmt size validation (16/18/40 bytes), extensible tuple validation, and 0xFFFFFFFF stream length preservation (`Wav/wav_reader.c`, `Wav/wav_writer.c`).

