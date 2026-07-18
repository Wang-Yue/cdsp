# CDSP Engine State Management Architecture

This document provides a detailed, step-by-step technical guide on how the CDSP Engine manages state transitions, synchronization flags, thread interactions, and queue teardowns. It is designed to help you navigate the "state flag" relationships when modifying startup, playback, or shutdown paths.

---

## 1. Key State Variables & Structs Reference

The state is managed across three layers: the Engine controller (`dsp_engine_t`), the Session builder/teardown (`dsp_session_t`), and the Inter-thread synchronizer (`engine_shared_state_t`).

```mermaid
graph TD
    dsp_engine_t -->|"Session handle (Pointer)"| dsp_session_t
    dsp_engine_t -->|"State persistence manager"| engine_state_manager_t
    dsp_session_t -->|"Inter-thread Shared State"| engine_shared_state_t
```

### 1.1. Inter-Thread Level (`engine_shared_state_t`)
Defined in [engine_shared_state.c](Engine/engine_shared_state.c). This struct is shared directly among the Capture, Processing, and Playback threads and is 100% lock-free.

| Field Name | Type | Purpose | Concurrency Model |
| :--- | :--- | :--- | :--- |
| `state_raw` | `_Atomic uint8_t` | Encodes `processing_state_t` (`INACTIVE`, `RUNNING`, `PAUSED`, `STALLED`). When set to `INACTIVE`, it serves as the global stop signal. | Lock-free atomic reads (`acquire` ordering) and writes (`release` ordering). |
| `stop_once` | `_Atomic bool` | A latch/flag indicating whether a stop sequence has been initiated. Prevents multiple stop requests from colliding. | Checked and set atomically via Compare-And-Swap (CAS) `atomic_compare_exchange_strong_explicit`. |
| `stop_reason` | `processing_stop_reason_t` | A 264-byte struct containing the type of stop, error messages, or format change samplerates. | Read and written under protection of `stop_reason_mutex`. |
| `stop_reason_mutex` | `pthread_mutex_t` | Mutex protecting `stop_reason` against concurrent read/write data races. | C11 Mutex Lock (isolated from hot-path processing loop). |
| `captured_queue` | `audio_sync_queue_t*` | Sync queue from Capture -> Processing. | Lock-free SPSC + OS semaphore. |
| `processed_queue` | `audio_sync_queue_t*` | Sync queue from Processing -> Playback. | Lock-free SPSC + OS semaphore. |
| `last_capture_time_ns` | `_Atomic uint64_t` | Telemetry timestamp of the last successfully captured chunk in nanoseconds. Checked by the external watchdog to detect driver freezes. | Lock-free atomic reads and writes (`relaxed` ordering). |

---

### 1.2. Session Level (`dsp_session_t`)
Defined in [dsp_session_internal.h](Engine/dsp_session_internal.h). Manages resource lifetimes (backends, resampler, threads, chunk pools).

| Field Name | Type | Purpose | Concurrency Model |
| :--- | :--- | :--- | :--- |
| `threads_created` | `bool` | Set to `true` if all worker threads spawned successfully. If `false`, teardown skips calling `pthread_join` to prevent hangs. | Thread-confined (touched only on the main controller thread). |
| `config_mutex` | `pthread_mutex_t` | Guards access to the `current_config` pointer. | C11 Mutex Lock. |
| `current_config` | `dsp_config_t*` | Pointer to the active session config. | Protected by `config_mutex`. |

---

### 1.3. Controller Level (`dsp_engine_t`)
Defined in [dsp_engine.c](Engine/dsp_engine.c). The top-level controller interfacing with the Server and user commands, structured into domain sub-groups (`session`, `buffers`, `config`).

| Field Name | Type | Purpose | Concurrency Model |
| :--- | :--- | :--- | :--- |
| `state_mutex` | `pthread_mutex_t` | Serializes configuration changes, volumes, and status queries. | C11 Mutex Lock. |
| `session.last_stop_reason` | `processing_stop_reason_t` | Persists the stop reason (e.g. EOF or Error) returned by `dsp_session_stop_and_free()` after session destruction. If empty/none, its type is `STOP_REASON_NONE`. | Protected by `state_mutex`. |
| `config.in_progress` | `_Atomic bool` | True if a configuration change or reload is actively running. Status queries check this flag to return `STARTING` without blocking on `state_mutex`. | Lock-free atomic reads and writes. |

---

### 1.4. State Persistence Level (`engine_state_manager_t`)
Defined in [engine_state_manager.h](Engine/engine_state_manager.h). Manages fader volume/mute settings, config path, and state file serialization.

| Field Name | Type | Purpose | Concurrency Model |
| :--- | :--- | :--- | :--- |
| `fader_volumes` | `double[5]` | Target volume levels in dB for audio faders. | Protected by `mutex`. |
| `fader_mutes` | `bool[5]` | Target mute flags for audio faders. | Protected by `mutex`. |
| `state_file_path` | `char[1024]` | Path to state persistence file on disk. | Protected by `mutex`. |
| `active_config_path` | `char[1024]` | Path to active configuration file. | Protected by `mutex`. |
| `change_counter` | `uint64_t` | Monotonic counter tracking state modifications. | Protected by `mutex`. |

---

### 1.5. Atomic Variables & Accessing Threads Justification

Atomic variables in the engine are strictly restricted to fields that are either accessed on the real-time audio hot path or required for non-blocking asynchronous coordination. Using atomic operations for these fields avoids acquiring OS mutex locks inside high-priority audio threads (`EngineCaptureLoop`, `EngineProcessingLoop`, `EnginePlaybackLoop`), preventing priority inversion and audio dropouts/underruns.

| Atomic Variable | Location | Accessing Threads | Necessity & Justification |
| :--- | :--- | :--- | :--- |
| `state_raw` (`_Atomic uint8_t`) | `engine_shared_state_t` | **Writers**: Capture loop (RUNNING/PAUSED/STALLED), Playback loop (INACTIVE on exit).<br>**Readers**: Capture/Processing/Playback loops, Main thread (`poll`/status query). | Audio loops check `state_raw` on every chunk/iteration to detect pause or stop states. Must be lock-free to prevent blocking audio loops on status checks. |
| `stop_once` (`_Atomic bool`) | `engine_shared_state_t` | **Writers/Readers**: Any thread requesting a session stop (Capture, Processing, Playback, or Main controller thread). | Serves as a wait-free CAS gate (`atomic_compare_exchange_strong_explicit`). Ensures the first thread encountering EOF or hardware error sets the root-cause stop reason and initiates teardown without lock contention. |
| `last_capture_time_ns` (`_Atomic uint64_t`) | `engine_shared_state_t` | **Writer**: Capture thread (every captured chunk read).<br>**Reader**: Main thread (`cdsp_engine_poll` external watchdog check). | Written by the Capture thread on every audio chunk. Relaxed atomic load/store allows the main thread to poll hardware stall status without locking the Capture thread. |
| `resampler_ratio` (`_Atomic double`) | `engine_shared_state_t` | **Writer**: Playback thread (rate-adjust controller).<br>**Reader**: Processing thread (`processing_loop_resample`). | Both Playback and Processing threads are real-time audio loops. Atomic double allows lock-free propagation of clock drift correction between threads without mutex locks. |
| `next_pipeline` (`_Atomic(pipeline_t*)`) | `engine_processing_loop_t` | **Writer**: Main thread (`dsp_session_reload_config`).<br>**Reader**: Processing thread (`processing_loop_check_pipeline_swap`). | Checked by the Processing thread on every chunk and 0-frame tick. Atomic exchange allows non-blocking DSP filter pipeline hot-reloads without locking the Processing thread. |
| `is_shutdown` (`_Atomic bool`) | `audio_sync_queue_t` | **Writer**: Producer thread (Capture/Processing calling `shutdown`).<br>**Reader**: Consumer thread (Processing/Playback calling `dequeue_blocking`). | Checked inside the audio thread blocking dequeue loop to detect queue shutdown cleanly without acquiring mutex locks on the SPSC queue path. |
| `config.in_progress` (`_Atomic bool`) | `dsp_engine_impl_t` | **Writer**: Main thread (`dsp_engine_set_config_json`).<br>**Reader**: External HTTP/WebSocket API server threads (`dsp_engine_get_status`). | Allows external API status queries to return `PROCESSING_STATE_STARTING` instantly without blocking on `state_mutex` while the main thread performs long-running JSON parsing or device creation. |

---

### 1.6. Mutex Isolation & Non-Audio-Thread Concurrency Model

All mutexes in the CDSP Engine are strictly isolated to control paths, RPC/API handlers, or out-of-band error publishing. **No mutex is ever acquired on the real-time steady-state audio hot path** (Capture, Processing, Playback loops), satisfying real-time audio guarantees.

| Mutex Name | Location | Accessing Threads | Purpose & Deadlock Prevention Analysis |
| :--- | :--- | :--- | :--- |
| `stop_reason_mutex` | `engine_shared_state_t` | Threads calling `engine_shared_state_request_stop()` or `get_stop_reason()` (Capture, Playback, Processing, Main thread). | Protects `stop_reason` (a 264-byte payload struct). **Leaf lock**: never acquires any other lock inside its critical section and is locked at most **once** during session teardown/stop publication. |
| `config_mutex` | `dsp_session_t` | Main controller thread calling `dsp_session_get_config()` or `dsp_session_reload_config()`. | Guards access to `current_config`. **Leaf lock**: never acquires any other mutex while held. Used exclusively for control RPCs; never accessed by audio loops. |
| `state_mutex` | `dsp_engine_impl_t` | Main thread executing API commands (`set_config_json`, `stop`, `set_fader_volume`, `get_status`, `poll`, `get_samples`, `get_spectrum`). | Serializes top-level state transitions, session building/destruction, and historical stop reasons. **Top-level controller lock**: acquires leaf locks in a strict top-down hierarchy. Audio loops operate entirely inside `dsp_session_t` and `engine_shared_state_t`, completely isolated from `state_mutex`. |
| `mutex` | `engine_state_manager_t` | Main/API threads calling `set_fader_volume`, `set_fader_mute`, `get_fader_volume`, `set_config_path`, `save_if_needed`. | Protects fader volume/mute settings, paths, dirty flags, and change counters as unified atomic transactions. **Leaf lock with recursive type** (`PTHREAD_MUTEX_RECURSIVE`): nested internal calls on the same thread cannot self-deadlock. Audio threads update volume smoothly via `processing_parameters_t` (atomic gain factors) and **never** touch `engine_state_manager_t`. |

#### Deadlock Prevention Architecture Rules
To mathematically guarantee freedom from deadlocks across all thread interactions, the engine enforces four strict lock hierarchy rules:

1. **Strict 2-Tier Lock Ordering Hierarchy**:
   - **Level 1 (Top-Level Controller)**: `state_mutex` (`dsp_engine_impl_t`)
   - **Level 2 (Leaf Locks)**: `config_mutex`, `stop_reason_mutex`, `engine_state_manager_t.mutex`
   - Locking is strictly single-directional (Level 1 $\rightarrow$ Level 2). A Level 2 leaf lock **never** attempts to acquire `state_mutex` or any other Level 2 leaf lock.
2. **Mutual Independence of Leaf Locks**:
   - `config_mutex`, `stop_reason_mutex`, and `mgr->mutex` are completely independent. No function ever holds more than one leaf lock simultaneously, eliminating circular wait dependencies.
3. **Re-entrant Self-Deadlock Immunity**:
   - `engine_state_manager_t.mutex` is created as a `PTHREAD_MUTEX_RECURSIVE` lock. Internal methods (e.g. `save_if_needed()` calling `get_config_path()` or `get_fader_volume()`) executed on the main/API thread can safely re-acquire the lock without self-deadlocking.
4. **Audio Thread Lock Exclusion**:
   - Steady-state audio loops (`EngineCaptureLoop`, `EngineProcessingLoop`, `EnginePlaybackLoop`) never acquire `state_mutex`, `config_mutex`, or `mgr->mutex`. The only lock touched by an audio loop is `stop_reason_mutex`, which is a leaf lock invoked strictly once on error/EOF exit paths outside the audio streaming loop.

---

## 2. State Transition Matrices

### 2.1. Core Processing States (`processing_state_t`)
The raw state of the engine is stored in `state_raw` inside `engine_shared_state_t`:

```mermaid
stateDiagram-v2
    [*] --> INACTIVE
    INACTIVE --> STARTING : set_config
    STARTING --> RUNNING : spawn_threads / capture_starts
    RUNNING --> PAUSED : silence_timeout
    PAUSED --> RUNNING : signal_detected
    RUNNING --> STALLED : watchdog_timeout (>0.5s)
    STALLED --> RUNNING : read_success
    RUNNING --> INACTIVE : abort / error / stop
    PAUSED --> INACTIVE : abort / error / stop
    STALLED --> INACTIVE : abort / error / stop
```

* **`INACTIVE`**: Threads are stopped or in the process of shutting down. `engine_shared_state_should_stop()` returns `true`.
* **`STARTING`**: The engine is active but in the process of rebuilding/restarting its session (e.g. opening device backends, pre-filling silence, spawning threads). Status queries return this state instantly without blocking.
* **`RUNNING`**: Active audio capture and playback.
* **`PAUSED`**: Audio levels are below the silence threshold. Worker loops still run but backends are paused to yield CPU time.
* **`STALLED`**: The watchdog detected that no chunks have arrived from the hardware capture backend for too long, indicating driver or hardware starvation.

---

## 3. Thread Lifecycles & State Scenarios

The engine progresses through several lifecycle phases, moving between states according to hardware feedback, queue levels, and user inputs.

---

### 3.1. Startup & Initialization Flow
This scenario starts when a new configuration is applied. The engine goes into `STARTING` state while it initializes synchronously, then spawns the real-time audio threads. The threads open device backends asynchronously, and once capture starts successfully, it transitions the state to `RUNNING`.

```mermaid
sequenceDiagram
    autonumber
    participant U as UI / Client
    participant E as DSPEngine Controller
    participant B as Session Builder
    participant S as Shared State
    participant C as Capture Thread
    participant K as Playback Thread

    U->>E: set_config(json)
    Note over E: Set config_in_progress = true<br/>(Status queries now return STARTING)
    E->>B: build_and_start(config)
    Note over B: Allocates backends & pipeline structures<br/>Initializes state_raw to STARTING
    B->>C: Spawn Capture thread
    B->>K: Spawn Playback thread
    B-->>E: Returns Session handle
    Note over E: Set config_in_progress = false<br/>(Status queries read STARTING from state_raw)
    E-->>U: Config changed OK

    par Capture Loop Setup
        C->>C: Opens capture device backend
        Note over C: If failed: requests stop (CAPTURE_ERROR)
    and Playback Loop Setup
        K->>K: Opens playback device backend
        K->>K: Pre-fills DAC with silence frames
        Note over K: If failed: requests stop (PLAYBACK_ERROR)
    end

    Note over C: Once capture open succeeds:
    C->>S: set_state(RUNNING)
    Note over S: state_raw becomes RUNNING<br/>(Status queries now return RUNNING)
```

1. **Staging & Lock-Free Status Indicator**:
   - `dsp_engine_set_config_json()` sets `config_in_progress` to `true` (see [dsp_engine.c](file:///Users/wangyue/cdsp/Engine/dsp_engine.c)). This allows client WebSocket/HTTP poll queries to immediately return `PROCESSING_STATE_STARTING` lock-free without blocking on `state_mutex`.

2. **Allocating Session Core & Mutex**:
   - `engine_session_build_and_start()` allocates the `dsp_session_t` container and initializes `config_mutex` (see [engine_session_builder.c](file:///Users/wangyue/cdsp/Engine/engine_session_builder.c)).

3. **Shared State & DSD/DoP Helpers Setup**:
   - `engine_session_build_shared_state_and_dop()` allocates `engine_shared_state_t` (initializing SPSC queues `captured_queue` and `processed_queue`, and `state_raw` to `PROCESSING_STATE_STARTING`).
   - If DoP is enabled on capture/playback, it allocates `dop_decoder_t`. If DSD is enabled on playback, it allocates `dsd_encoder_t`.

4. **Resampler Sizing & Allocation**:
   - The resampler ratio is calculated. If the configured capture rate differs from the target pipeline rate, `resampler_create_from_config()` allocates a synchronous, asynchronous sinc, or asynchronous poly resampler.
   - The effective playback chunk size is determined based on the resampler's maximum output frame expansion.

5. **Audio Backend Factories (Without Opening)**:
   - `engine_session_build_backends()` calls the backend factory to allocate `capture_backend_t` and `playback_backend_t` handles (e.g. CoreAudio, ALSA, ASIO, File, or Generator). No hardware syscalls are made here; only handle descriptors are created.

6. **DSP Processing Pipeline & Scratch Chunks**:
   - `engine_session_build_pipeline_and_scratch()` parses filter configurations, creates the step pipeline (`pipeline_t`), and allocates the `resampler_scratch` and `pipeline_scratch` audio chunks used for processing.

7. **Thread Chunk Pools Sizing**:
   - Pre-allocates two round-robin chunk pools (`capture_chunk_pool` and `processing_scratch_pool`) to completely avoid memory allocations on the audio thread hot path during steady-state processing.

8. **Spawning Worker Threads**:
   - `engine_session_spawn_worker_threads()` spawns the Capture thread, Processing thread, and Playback thread in parallel.
   - If any thread spawning fails, the builder manually stops and joins already-created worker threads, triggers session destruction via `dsp_session_stop_and_free()`, and returns `NULL` to abort the start.

9. **Asynchronous Hardware Open & Prefill**:
   - **Playback Loop Setup**: The Playback thread opens the playback backend device and pre-fills it with silence frames (PCM zero-fill or DSD silence pattern) to prevent immediate buffer underrun errors on startup.
   - **Capture Loop Setup**: The Capture thread opens the capture backend device and starts reading frames.
   - If either thread fails to open the device asynchronously, it requests an abort via `engine_shared_state_request_stop()` with the specific error type (e.g., `STOP_REASON_CAPTURE_ERROR` or `STOP_REASON_PLAYBACK_ERROR`), which immediately transitions the raw state to `INACTIVE`.

10. **Transition to `RUNNING`**:
    - Once the capture thread successfully opens its device and reads the first frame, it sets `state_raw` to `PROCESSING_STATE_RUNNING`, notifying the controller that the session is streaming.

---

### 3.2. Steady-State Audio Loops
Once started, audio chunks flow continuously through SPSC queues synchronized by low-overhead OS semaphores.

```mermaid
graph LR
    H_In[Capture Mic/Line] -->|hardware read| C[Capture Loop]
    C -->|pool chunk| S_Cap[captured_queue SPSC]
    S_Cap -->|blocking dequeue| P[Processing Loop]
    P -->|process pipeline| S_Proc[processed_queue SPSC]
    S_Proc -->|blocking dequeue| K[Playback Loop]
    K -->|hardware write| H_Out[Playback DAC/Line]
```

* **Capture Loop**: Obtains a pre-allocated chunk from `capture_pool`, reads PCM/DSD from the device backend, runs metering/silence checks, and pushes to `captured_queue`. It then signals `captured_queue`'s semaphore.
* **Processing Loop**: Blocks on `captured_queue`'s semaphore. Once awakened, it dequeues the chunk, runs it through the DSP pipeline (resampler, mixers, channels, volume/mute), obtains a scratch chunk from the pool, copies the output, and enqueues it to `processed_queue`, signaling its semaphore.
* **Playback Loop**: Blocks on `processed_queue`'s semaphore. Once awakened, it dequeues the processed chunk, runs the rate-adjust controller, and writes the frame samples to the physical playback backend.

---

### 3.3. Silence Auto-Pause & Resume Flow
If silence detection is enabled, the capture loop auto-pauses the downstream pipeline to save CPU and stops writing to the output device when silence is sustained.

```mermaid
sequenceDiagram
    autonumber
    participant C as Capture Thread
    participant S as Shared State
    participant B as Backend Devices
    participant K as Playback Thread

    Note over C: Peak levels fall below silence_threshold_db
    Note over C: silence_timeout_seconds elapses
    C->>S: set_state(PAUSED)
    C->>B: Set playback & capture backends is_paused = true
    Note over C: Enqueueing of captured chunks stops
    Note over K: processed_queue becomes empty<br/>Blocks on processed semaphore

    Note over C: New audio signal arrives (peak > threshold)
    C->>S: set_state(RUNNING)
    C->>B: Set playback & capture backends is_paused = false
    C->>S: Enqueueing of captured chunks resumes
    Note over K: Awakens and writes audio to DAC again
```

1. **Silence Detection**:
   - The capture loop monitors the peak levels of each chunk.
   - If the level stays below the threshold for longer than the timeout, the silence counter updates.
2. **Auto-Pause Transition**:
   - The capture thread updates the state to `PROCESSING_STATE_PAUSED`.
   - It pauses both capture and playback backends. In paused mode, backends yield CPU cycles.
   - The capture thread stops pushing active audio chunks to `captured_queue`.
   - **Periodic 0-Frame Ticks**: To prevent configuration hot-reloads (pipeline swaps) or parameter updates (volume/mute) from being delayed indefinitely during silence, the capture thread periodically enqueues empty chunks (`valid_frames == 0`) downstream every 200ms.
   - The processing thread wakes up on these 0-frame chunks, checks and performs any pending pipeline swaps, and propagates them downstream.
   - The playback thread blocks on `processed_queue` and drops 0-frame chunks immediately to bypass hardware writes and rate controllers.
3. **Signal Auto-Resume**:
   - When a loud chunk is read (above the threshold), the silence counter resets.
   - The capture thread sets the state back to `PROCESSING_STATE_RUNNING`.
   - Both backends are unpaused, and chunk pushing resumes, waking up the downstream threads.

---

### 3.4. Watchdog Stall & Recovery Flow
The watchdog monitors hardware starvation. If the driver halts or the device is disconnected without throwing a direct backend error, the engine transitions to `STALLED`.

```mermaid
sequenceDiagram
    autonumber
    participant H as Hardware / Driver
    participant C as Capture Thread
    participant S as Shared State
    participant M as Main Thread (Controller)

    Note over C: read() blocks infinitely inside driver
    Note over M: cdsp_engine_poll() triggers periodically
    M->>S: Get last capture time
    Note over M: elapsed since last capture > 0.5s
    M->>S: set_state(STALLED)
    Note over M: Logs warning: "External watchdog: capture device stalled"
    
    Note over H: Driver recovers, read() returns chunk
    C->>S: Set last capture time
    C->>S: set_state(RUNNING)
    Note over C: Logs info: "Capture recovered from stall (external)"
```

1. **Stall Detection**:
   - During normal reads, the capture thread updates the shared timestamp `last_capture_time_ns` in the shared state every time a chunk is read.
   - If the capture backend read call blocks infinitely (e.g., driver freeze or hardware disconnect), the capture thread remains blocked inside `capture_backend_read` and cannot execute internal checks.
   - The main controller thread running `cdsp_engine_poll()` reads the shared `last_capture_time_ns` periodically.
   - If the elapsed time since `last_capture_time_ns` exceeds `0.5` seconds, the main thread transitions `state_raw` to `PROCESSING_STATE_STALLED` and logs a warning. This ensures status queries show `STALLED` even when the capture thread is permanently hung.
2. **Stall Recovery**:
   - The capture thread keeps waiting/reading. If the device/driver recovers and successfully delivers a new chunk, the capture thread updates `last_capture_time_ns` in shared state.
   - The capture thread checks if the shared state is currently `PROCESSING_STATE_STALLED`. If so, it transitions it back to `PROCESSING_STATE_RUNNING` and logs a recovery message.

---

### 3.5. Graceful EOF Teardown (Queue Drain)
Used when a finite file input completes. The goal is to let the remaining audio drain through all buffers to the DAC.

```mermaid
sequenceDiagram
    autonumber
    participant C as Capture Thread
    participant P as Processing Thread
    participant K as Playback Thread
    participant S as Shared State

    Note over C: Reaches EOF in file
    C->>S: request_stop(STOP_REASON_DONE)
    Note over S: CAS stop_once -> true<br/>stop_reason = DONE<br/>state_raw remains RUNNING
    C->>S: shutdown_captured_queue()
    Note over C: Exits run loop & terminates
    
    Note over P: dequeue_captured_blocking() returns remaining chunks
    Note over P: captured_queue becomes empty
    P->>S: dequeue_captured_blocking() returns NULL
    P->>S: shutdown_processed_queue()
    Note over P: Exits run loop & terminates

    Note over K: dequeue_processed_blocking() returns remaining chunks
    Note over K: processed_queue becomes empty
    K->>S: dequeue_processed_blocking() returns NULL
    Note over K: Drains hardware DAC ring buffer (3 seconds max)
    K->>S: set_state(INACTIVE)
    Note over K: Exits run loop & terminates
```

1. **Capture Loop Reaches EOF**: 
   - Calls `engine_shared_state_request_stop(state, STOP_REASON_DONE)`.
   - CAS on `stop_once` succeeds. `stop_reason` is set to `STOP_REASON_DONE`.
   - **Crucial**: The state is **not** set to `INACTIVE`. `state_raw` remains `RUNNING`.
   - Calls `audio_sync_queue_shutdown(captured_queue)`.
2. **Processing Loop Drains Capture Queue**:
   - Keeps dequeuing chunks from `captured_queue` and processing them.
   - Once all chunks are drained, `dequeue_captured_blocking` returns `NULL`.
   - The processing thread exits its `while(1)` loop, then calls `engine_shared_state_shutdown_processed_queue(state)`.
3. **Playback Loop Drains Playback Queue**:
   - Dequeues and plays all remaining processed chunks.
   - Once `processed_queue` is empty, `dequeue_processed_blocking` returns `NULL`.
   - The playback thread runs `playback_loop_drain_hardware_buffer` to wait for the DAC buffer to hit `0`.
   - Sets the state to `INACTIVE` via `engine_shared_state_set_state(state, PROCESSING_STATE_INACTIVE)`.
   - Playback thread terminates.

---

### 3.6. Immediate Abort Teardown
Used when a thread crashes, a hardware device is disconnected, or the user clicks "Stop". Threads must wake up and abort immediately.

```mermaid
sequenceDiagram
    autonumber
    participant K as Playback Thread
    participant S as Shared State
    participant P as Processing Thread
    participant C as Capture Thread

    Note over K: Hardware write error detected
    K->>S: request_stop(STOP_REASON_PLAYBACK_ERROR)
    Note over S: CAS stop_once -> true<br/>stop_reason = ERROR<br/>state_raw = INACTIVE
    S->>S: shutdown_captured_queue()
    S->>S: shutdown_processed_queue()
    Note over K: Exits run loop & terminates

    Note over P: Blocked in dequeue or enqueue
    P->>S: dequeue/enqueue detects should_stop() (INACTIVE)
    Note over P: Aborts immediately
    Note over P: Exits run loop & terminates

    Note over C: Blocked in enqueue_captured
    C->>S: enqueue detects should_stop() (INACTIVE)
    Note over C: Aborts immediately
    Note over C: Exits run loop & terminates
```

1. **Playback Thread Detects Error**:
   - Calls `engine_shared_state_request_stop(state, STOP_REASON_PLAYBACK_ERROR)`.
   - CAS on `stop_once` succeeds. `stop_reason` is set to the error.
   - **Crucial**: State is immediately changed to `INACTIVE`.
   - **Crucial**: Both `captured_queue` and `processed_queue` are immediately shut down.
2. **Unblocking Worker Threads**:
   - The Capture and Processing threads might be blocked waiting on semaphores (sleeping) or blocked waiting to push to full queues (spinning).
   - Calling `shutdown` on the queues wakes up all semaphores immediately.
   - The threads check `engine_shared_state_should_stop()`, which returns `true` (since state is `INACTIVE`).
   - All loops break and threads terminate immediately.
3. **Controller Teardown**:
   - The controller joins all terminated threads and frees session allocations safely.

---

## 4. The CAS Race-Condition Safety Gate

One of the trickiest parts of shutdown is when multiple threads encounter errors simultaneously. For example:
- The capture thread encounters a read timeout error.
- At the same time, the playback thread gets a buffer underrun/write error.
- Simultaneously, the user clicks "Stop" in the UI.

Without a gate, these threads would overwrite the stop reason and attempt to shut down queues repeatedly, causing crashes or double-frees.

```c
  bool expected = false;
  if (atomic_compare_exchange_strong_explicit(&state->stop_once, &expected,
                                              true, memory_order_acq_rel,
                                              memory_order_acquire)) {
      // WINNER: Only this block executes once.
      state->stop_reason = reason;
      ...
  } else {
      // LOSER: Stop has already been requested by someone else.
      ...
  }
```

* **The First Thread (Winner)**: Atomic compare-exchange finds `stop_once` is `false`. It atomically updates it to `true` and returns `true`. It sets the initial, most accurate stop reason.
* **Subsequent Threads (Losers)**: Find `stop_once` is already `true`. They enter the `else` branch.
  - If a graceful EOF was previously requested (`stop_reason == STOP_REASON_DONE`), but a new thread requests an immediate abort (`reason.type != STOP_REASON_DONE`), the loser branch **overrides** the stop reason and forces an immediate queue shutdown.
  - Otherwise, it does not overwrite the stop reason to preserve the first error root cause.

---

## 5. Summary Cheat Sheet for Debugging

| Symptom | Probable Cause | Action |
| :--- | :--- | :--- |
| **Shutdown Hangs (Infinite Join)** | Playback thread is blocked waiting on `processed_queue`'s semaphore. | Check if `engine_shared_state_request_stop` forgot to shut down `processed_queue` on error path. |
| **Processing Spins Endless 1ms Sleeps** | EOF was requested, but playback thread terminated early, leaving the queue full. `should_stop` check was skipped because stop reason was `DONE`. | Ensure `should_stop` breaks the enqueue loop immediately regardless of the stop reason. |
| **Torn Stop Reasons / Corrupt Msg** | Thread read `state->stop_reason` concurrently while another thread was writing it. | Ensure all reads/writes of `stop_reason` are wrapped by `stop_reason_mutex` and returned by value. |
| **Double Free on Config Change** | Controller freed `config` on failure, but `dsp_session_stop_and_free` also freed it because `core->current_config` was assigned. | Ensure ownership of the `config` struct is explicitly transferred and handled only by the session on builder exit. |
