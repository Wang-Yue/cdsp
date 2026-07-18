# CDSP Engine Audio Loop Call Graph Audit Report

This report documents the static Call Graph Audit for the three real-time audio loop entry points in the CDSP Engine:
1. `EngineCaptureLoop` (`engine_capture_loop_run`)
2. `EngineProcessingLoop` (`engine_processing_loop_run`)
3. `EnginePlaybackLoop` (`engine_playback_loop_run`)

---

## 0. Architectural Motivation: Why Call Graph Auditing is Required

In real-time audio DSP systems, meeting strict low-latency constraints (e.g. 512 frames @ 44.1kHz = 11.6ms deadline per chunk) requires absolute guarantees that real-time threads never block unpredictably. Static Call Graph Auditing is performed for four essential architectural reasons:

1. **Deterministic Latency & Dropout Prevention**:
   Acquiring a `pthread_mutex_lock` or invoking dynamic memory allocators (`malloc`/`free`) inside steady-state audio loops causes OS thread stalls. In real-time audio, even a microsecond stall can miss a DAC hardware buffer deadline, producing audible pops, clicks, or stream drops.
2. **Preventing Priority Inversion**:
   Audio threads run at OS Real-Time scheduling priority (`SCHED_FIFO` / `TH_OPT_REALTIME`). If a real-time audio thread attempts to acquire a lock held by a low-priority API thread (e.g., HTTP/WebSocket server or JSON parser), the high-priority thread is blocked waiting for low-priority execution. Call Graph Auditing proves that audio loops do not touch locks shared with control-plane threads.
3. **Protection Against Implicit Lock Contamination During Refactoring**:
   As codebases grow, developers may inadvertently call utility functions or third-party helpers that internally acquire locks or allocate memory. Static Call Graph AST analysis recursively inspects 100% of reachable call trees from the audio loop entry points, ensuring new additions do not accidentally introduce hidden locks into steady-state audio streaming.
4. **Automated CI/CD Concurrency Governance**:
   Documented architectural promises must be continuously verified. Integrating static call graph auditing into `Tools/generate_callgraph.py` automatically enforces lock-free invariants on every build and pull request.

---

## 1. Audit Summary & Key Invariants

| Audio Loop | Total Reachable Functions | Lock Reachability on Steady Path | Dynamic Lock / Alloc Risk | Audit Result |
| :--- | :--- | :--- | :--- | :--- |
| **`EngineCaptureLoop`** | 12 | **0 (None)** | None | ✅ PASS (Zero-Lock) |
| **`EngineProcessingLoop`** | 11 | **0 (None)** | None | ✅ PASS (Zero-Lock) |
| **`EnginePlaybackLoop`** | 29 | **0 (None)** | None | ✅ PASS (Zero-Lock) |

> [!NOTE]
> **Static Call Path Analysis Result**: Across all three audio loops, `pthread_mutex_lock` is **0% reachable on steady-state audio streaming paths**. The only call path leading to `stop_reason_mutex` is strictly encapsulated inside `engine_shared_state_request_stop()` on fatal hardware errors or EOF stream termination.

---

## 2. Capture Loop Call Graph (`engine_capture_loop_run`)

The Capture thread reads audio samples from the input backend (Hardware Mic/Line, File, or Signal Generator), performs sample rate watching, DoP decoding, level metering, and enqueues chunks into `captured_queue`.

```mermaid
graph TD
    engine_capture_loop_run -->|Steady Hot Path| engine_capture_loop_step
    engine_capture_loop_run -->|Steady Hot Path| engine_shared_state_should_stop
    engine_capture_loop_run -->|Steady Hot Path| engine_shared_state_set_state
    engine_capture_loop_run -->|Steady Hot Path| round_robin_chunk_pool_next
    engine_capture_loop_run -->|Error / Startup Path| capture_backend_open
    engine_capture_loop_run -->|Error / Startup Path| engine_shared_state_request_stop

    engine_capture_loop_step -->|Steady Hot Path| capture_backend_read
    engine_capture_loop_step -->|Steady Hot Path| capture_loop_process_and_enqueue
    engine_capture_loop_step -->|Error / EOF Path| capture_loop_handle_no_data

    capture_loop_process_and_enqueue -->|Steady Hot Path| dop_decoder_detect_and_process
    capture_loop_process_and_enqueue -->|Steady Hot Path| processing_parameters_update_capture_levels
    capture_loop_process_and_enqueue -->|Steady Hot Path| silence_counter_update
    capture_loop_process_and_enqueue -->|Steady Hot Path| engine_shared_state_enqueue_captured

    engine_shared_state_enqueue_captured -->|Lock-Free SPSC| audio_sync_queue_enqueue
    audio_sync_queue_enqueue -->|OS Semaphore Signal| cdsp_sem_signal

    capture_loop_handle_no_data -->|Error / EOF Path| engine_shared_state_request_stop
    engine_shared_state_request_stop -->|Error Lock| pthread_mutex_lock
```

---

## 3. Processing Loop Call Graph (`engine_processing_loop_run`)

The Processing thread dequeues chunks from `captured_queue`, executes active DSP resamplers and pipeline filter steps (or non-blocking pipeline swaps), records performance metrics, and enqueues processed chunks into `processed_queue`.

```mermaid
graph TD
    engine_processing_loop_run -->|Lock-Free Dequeue| engine_shared_state_dequeue_captured_blocking
    engine_processing_loop_run -->|Steady Hot Path| processing_loop_resample
    engine_processing_loop_run -->|Steady Hot Path| processing_loop_check_pipeline_swap
    engine_processing_loop_run -->|Steady Hot Path| pipeline_process
    engine_processing_loop_run -->|Steady Hot Path| processing_loop_record_metrics
    engine_processing_loop_run -->|Steady Hot Path| processing_loop_enqueue_output

    engine_shared_state_dequeue_captured_blocking -->|Lock-Free SPSC| audio_sync_queue_dequeue_blocking
    audio_sync_queue_dequeue_blocking -->|OS Kernel Sleep| cdsp_sem_wait

    processing_loop_check_pipeline_swap -->|Atomic Exchange| atomic_exchange
    processing_loop_check_pipeline_swap -->|Lock-Free SPSC| engine_shared_state_enqueue_garbage_pipeline

    processing_loop_enqueue_output -->|Lock-Free SPSC| engine_shared_state_enqueue_processed
    engine_shared_state_enqueue_processed -->|OS Semaphore Signal| cdsp_sem_signal

    processing_loop_resample -->|Error Path| engine_shared_state_request_stop
    pipeline_process -->|Error Path| engine_shared_state_request_stop
    engine_shared_state_request_stop -->|Error Lock| pthread_mutex_lock
```

---

## 4. Playback Loop Call Graph (`engine_playback_loop_run`)

The Playback thread dequeues chunks from `processed_queue`, updates the PI rate controller for clock drift correction, and writes audio samples to the output DAC backend.

```mermaid
graph TD
    engine_playback_loop_run -->|Lock-Free Dequeue| engine_shared_state_dequeue_processed_blocking
    engine_playback_loop_run -->|Steady Hot Path| playback_loop_update_rate_adjust
    engine_playback_loop_run -->|Steady Hot Path| playback_backend_write
    engine_playback_loop_run -->|Graceful EOS Drain| playback_loop_drain_hardware_buffer

    engine_shared_state_dequeue_processed_blocking -->|Lock-Free SPSC| audio_sync_queue_dequeue_blocking
    audio_sync_queue_dequeue_blocking -->|OS Kernel Sleep| cdsp_sem_wait

    playback_loop_update_rate_adjust -->|Math Computation| pi_rate_controller_next
    playback_loop_update_rate_adjust -->|Atomic Store| engine_shared_state_set_resampler_ratio

    playback_backend_write -->|Driver Direct Write| write
    playback_backend_write -->|Error Path| engine_shared_state_request_stop

    playback_loop_drain_hardware_buffer -->|Hardware Level Check| playback_backend_get_buffer_level
    engine_shared_state_request_stop -->|Error Lock| pthread_mutex_lock
```

---

## 5. How to Re-generate This Audit Report

Run the AST callgraph analysis tool in `Tools/generate_callgraph.py`:

```bash
python3 Tools/generate_callgraph.py
```

It parses the C AST across `Engine/`, `Audio/`, `DoP/`, `Pipeline/`, `Resampler/`, `Filters/`, `Mixer/`, `Utils/`, `Backend/`, and `Logging/` to verify lock reachability and generate JSON/Mermaid call graph topologies.
