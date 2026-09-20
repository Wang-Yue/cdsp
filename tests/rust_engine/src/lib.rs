use camillalib::{
    config, ControllerMessage, ExitState, ProcessingState, SharedConfigs, StatusStructs, StopReason,
};
use crossbeam_channel::{bounded, Sender};
use log::{debug, trace, Level, LevelFilter, Metadata, Record};
use parking_lot::Mutex;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

// ==============================================================================
// Log Handler (matches C log callback)
// ==============================================================================

pub type CdspLogCallback =
    Option<unsafe extern "C" fn(level: *const c_char, label: *const c_char, msg: *const c_char, user_data: *mut c_void)>;

struct RustLogger {
    callback: Mutex<Option<(unsafe extern "C" fn(*const c_char, *const c_char, *const c_char, *mut c_void), usize)>>,
}

static LOGGER: RustLogger = RustLogger {
    callback: Mutex::new(None),
};
static LOGGER_INITIALIZED: AtomicBool = AtomicBool::new(false);

impl log::Log for RustLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= log::max_level()
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let cb_opt = *self.callback.lock();
        if let Some((cb, user_data_ptr)) = cb_opt {
            let level_str = match record.level() {
                Level::Error => "ERROR\0",
                Level::Warn => "WARN\0",
                Level::Info => "INFO\0",
                Level::Debug => "DEBUG\0",
                Level::Trace => "TRACE\0",
            };

            let c_target = std::ffi::CString::new(record.target()).unwrap_or_default();
            let msg_str = format!("{}", record.args());
            let c_msg = match std::ffi::CString::new(msg_str.clone()) {
                Ok(s) => s,
                Err(_) => {
                    let clean = msg_str.replace('\0', " ");
                    std::ffi::CString::new(clean).unwrap_or_default()
                }
            };

            unsafe {
                cb(
                    level_str.as_ptr() as *const c_char,
                    c_target.as_ptr(),
                    c_msg.as_ptr(),
                    user_data_ptr as *mut c_void,
                );
            }
        }
    }

    fn flush(&self) {}
}

fn init_logger_if_needed() {
    if !LOGGER_INITIALIZED.swap(true, Ordering::SeqCst) {
        let _ = log::set_logger(&LOGGER);
        log::set_max_level(LevelFilter::Info);
    }
}

// ==============================================================================
// C API Data Structures (matching include/cdsp/cdsp_pub_types.h)
// ==============================================================================

pub struct CamillaEngine {
    tx_command: Sender<ControllerMessage>,
    status_structs: StatusStructs,
    active_config: Arc<Mutex<Option<config::Configuration>>>,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspProcessingState {
    Inactive = 0,
    Starting = 1,
    Running = 2,
    Paused = 3,
    Stalled = 4,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspStopReasonType {
    None = 0,
    Done = 1,
    CaptureError = 2,
    PlaybackError = 3,
    CaptureFormatChange = 4,
    PlaybackFormatChange = 5,
    UnknownError = 6,
}

#[repr(C)]
pub struct CdspStopReason {
    pub reason_type: CdspStopReasonType,
    pub message: [c_char; 256],
    pub format_change_rate: c_int,
}

#[repr(C)]
pub struct CdspVuLevels {
    pub playback_rms: *mut f32,
    pub playback_peak: *mut f32,
    pub capture_rms: *mut f32,
    pub capture_peak: *mut f32,
    pub playback_channels: usize,
    pub capture_channels: usize,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspDeviceErrorType {
    None = 0,
    NotFound = 1,
    Busy = 2,
    Unknown = 3,
}

#[repr(C)]
pub struct CdspDeviceError {
    pub error_type: CdspDeviceErrorType,
    pub message: [c_char; 256],
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspBackendErrorType {
    Success = 0,
    ConfigParse = 1,
    DeviceNotFound = 2,
    DeviceBusy = 3,
    ConfigRead = 4,
    Unknown = 5,
}

#[repr(C)]
pub struct CdspBackendError {
    pub error_type: CdspBackendErrorType,
    pub message: [c_char; 256],
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspSpectrumSide {
    Capture = 0,
    Playback = 1,
}

#[repr(C)]
pub struct CdspSpectrum {
    pub frequencies: *mut f32,
    pub magnitudes: *mut f32,
    pub count: usize,
    pub error_message: [c_char; 128],
}

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CdspFader {
    Main = 0,
    Aux1 = 1,
    Aux2 = 2,
    Aux3 = 3,
    Aux4 = 4,
}

#[repr(C)]
pub struct CdspAudioSamples {
    pub channels: *mut *mut f32,
    pub channels_count: usize,
    pub frames: usize,
}

#[repr(C)]
pub struct CdspDeviceInfo {
    pub identifier: [c_char; 256],
    pub name: [c_char; 256],
    pub has_name: bool,
}

#[repr(C)]
pub struct CdspSamplerateCapability {
    pub samplerate: c_int,
    pub formats: *mut *mut c_char,
    pub formats_count: usize,
}

#[repr(C)]
pub struct CdspChannelCapability {
    pub channels: c_int,
    pub samplerates: *mut CdspSamplerateCapability,
    pub samplerates_count: usize,
}

#[repr(C)]
pub struct CdspDeviceCapabilitySet {
    pub mode: [c_char; 64],
    pub capabilities: *mut CdspChannelCapability,
    pub capabilities_count: usize,
}

#[repr(C)]
pub struct CdspDeviceDescriptor {
    pub name: [c_char; 256],
    pub description: [c_char; 256],
    pub capability_sets: *mut CdspDeviceCapabilitySet,
    pub capability_sets_count: usize,
}

// ==============================================================================
// Helper Functions
// ==============================================================================

fn fader_to_rust_index(fader: CdspFader) -> usize {
    match fader {
        CdspFader::Main => 0,
        CdspFader::Aux1 => 1,
        CdspFader::Aux2 => 2,
        CdspFader::Aux3 => 3,
        CdspFader::Aux4 => 4,
    }
}

// ==============================================================================
// Public C API Export Functions (cdsp_* matching include/cdsp/*.h)
// ==============================================================================

#[no_mangle]
pub extern "C" fn cdsp_set_log_callback(callback: CdspLogCallback, user_data: *mut c_void) {
    init_logger_if_needed();
    let mut lock = LOGGER.callback.lock();
    if let Some(cb) = callback {
        *lock = Some((cb, user_data as usize));
    } else {
        *lock = None;
    }
}

#[no_mangle]
pub extern "C" fn cdsp_set_log_level(level_str: *const c_char) {
    init_logger_if_needed();
    if level_str.is_null() {
        return;
    }
    let s = unsafe { CStr::from_ptr(level_str).to_string_lossy().to_lowercase() };
    let filter = match s.as_str() {
        "off" => LevelFilter::Off,
        "error" => LevelFilter::Error,
        "warn" | "warning" => LevelFilter::Warn,
        "info" => LevelFilter::Info,
        "debug" => LevelFilter::Debug,
        "trace" => LevelFilter::Trace,
        _ => LevelFilter::Info,
    };
    log::set_max_level(filter);
}

#[no_mangle]
pub extern "C" fn cdsp_engine_create() -> *mut CamillaEngine {
    init_logger_if_needed();
    camillalib::spectrum::request_spectrum_data();

    let (tx_command, rx_command) = bounded(10);
    let active_config = Arc::new(Mutex::new(None));
    let status_structs = StatusStructs::default();

    let engine = Box::new(CamillaEngine {
        tx_command: tx_command.clone(),
        status_structs: status_structs.clone(),
        active_config: active_config.clone(),
    });

    let status_structs_clone = status_structs.clone();
    std::thread::spawn(move || {
        let previous_config = Arc::new(Mutex::new(None));

        loop {
            trace!("Rust Engine: Wait for config");
            loop {
                let has_config = (*active_config.lock()).is_some();
                if has_config && rx_command.is_empty() {
                    break;
                }
                match rx_command.recv() {
                    Ok(ControllerMessage::ConfigChanged(new_conf, _)) => {
                        *active_config.lock() = Some(*new_conf);
                        camillalib::set_stop_reason(&status_structs_clone.status, StopReason::None);
                    }
                    Ok(ControllerMessage::Stop) => {
                        *active_config.lock() = None;
                    }
                    Ok(ControllerMessage::Exit) => return,
                    Err(_) => return,
                }
            }

            let shared_configs = SharedConfigs {
                active: active_config.clone(),
                previous: previous_config.clone(),
            };

            let exitstatus = camillalib::engine::run(
                shared_configs,
                status_structs_clone.clone(),
                rx_command.clone(),
            );
            debug!("Rust Engine: Processing ended with status {:?}", exitstatus);

            if let Ok(ExitState::Exit) = exitstatus {
                return;
            }
        }
    });

    Box::into_raw(engine)
}

#[no_mangle]
pub extern "C" fn cdsp_engine_free(engine: *mut CamillaEngine) {
    if !engine.is_null() {
        let eng = unsafe { Box::from_raw(engine) };
        let _ = eng.tx_command.send(ControllerMessage::Exit);
    }
}

#[no_mangle]
pub extern "C" fn cdsp_engine_poll(_engine: *mut CamillaEngine) {
    // Poll loop handled internally
}

#[no_mangle]
pub extern "C" fn cdsp_stop(engine: *mut CamillaEngine) {
    if !engine.is_null() {
        let eng = unsafe { &*engine };
        let _ = eng.tx_command.send(ControllerMessage::Stop);
    }
}

#[no_mangle]
pub extern "C" fn cdsp_set_config_json(
    engine: *mut CamillaEngine,
    json_str: *const c_char,
    out_err: *mut CdspBackendError,
) -> bool {
    if engine.is_null() || json_str.is_null() {
        return false;
    }
    let eng = unsafe { &*engine };
    let json_slice = unsafe { CStr::from_ptr(json_str).to_string_lossy() };
    log::info!("Set config: {}", json_slice);

    let conf: config::Configuration = match serde_json::from_str(&json_slice) {
        Ok(c) => c,
        Err(e) => {
            if !out_err.is_null() {
                unsafe {
                    (*out_err).error_type = CdspBackendErrorType::ConfigParse;
                    let err_bytes = e.to_string().into_bytes();
                    let len = err_bytes.len().min(255);
                    std::ptr::copy_nonoverlapping(err_bytes.as_ptr() as *const c_char, (*out_err).message.as_mut_ptr(), len);
                    (*out_err).message[len] = 0;
                }
            }
            return false;
        }
    };

    let impulse_cache = Default::default();
    match eng.tx_command.send(ControllerMessage::ConfigChanged(Box::new(conf), impulse_cache)) {
        Ok(_) => true,
        Err(e) => {
            if !out_err.is_null() {
                unsafe {
                    (*out_err).error_type = CdspBackendErrorType::Unknown;
                    let err_bytes = e.to_string().into_bytes();
                    let len = err_bytes.len().min(255);
                    std::ptr::copy_nonoverlapping(err_bytes.as_ptr() as *const c_char, (*out_err).message.as_mut_ptr(), len);
                    (*out_err).message[len] = 0;
                }
            }
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn cdsp_get_state(engine: *const CamillaEngine) -> CdspProcessingState {
    if engine.is_null() {
        return CdspProcessingState::Inactive;
    }
    let eng = unsafe { &*engine };
    let cap = eng.status_structs.capture.read();
    match cap.state {
        ProcessingState::Running => CdspProcessingState::Running,
        ProcessingState::Paused => CdspProcessingState::Paused,
        ProcessingState::Inactive => CdspProcessingState::Inactive,
        ProcessingState::Starting => CdspProcessingState::Starting,
        ProcessingState::Stalled => CdspProcessingState::Stalled,
    }
}

#[no_mangle]
pub extern "C" fn cdsp_get_stop_reason(engine: *const CamillaEngine, out_reason: *mut CdspStopReason) {
    if engine.is_null() || out_reason.is_null() {
        return;
    }
    let eng = unsafe { &*engine };
    let stat = eng.status_structs.status.read();
    let r = &stat.stop_reason;

    unsafe {
        (*out_reason).format_change_rate = 0;
        (*out_reason).message[0] = 0;

        match r {
            StopReason::None => (*out_reason).reason_type = CdspStopReasonType::None,
            StopReason::Done => (*out_reason).reason_type = CdspStopReasonType::Done,
            StopReason::CaptureError(msg) => {
                (*out_reason).reason_type = CdspStopReasonType::CaptureError;
                let b = msg.as_bytes();
                let len = b.len().min(255);
                std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, (*out_reason).message.as_mut_ptr(), len);
                (*out_reason).message[len] = 0;
            }
            StopReason::PlaybackError(msg) => {
                (*out_reason).reason_type = CdspStopReasonType::PlaybackError;
                let b = msg.as_bytes();
                let len = b.len().min(255);
                std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, (*out_reason).message.as_mut_ptr(), len);
                (*out_reason).message[len] = 0;
            }
            StopReason::CaptureFormatChange(rate) => {
                (*out_reason).reason_type = CdspStopReasonType::CaptureFormatChange;
                (*out_reason).format_change_rate = *rate as c_int;
            }
            StopReason::PlaybackFormatChange(rate) => {
                (*out_reason).reason_type = CdspStopReasonType::PlaybackFormatChange;
                (*out_reason).format_change_rate = *rate as c_int;
            }
            StopReason::UnknownError(msg) => {
                (*out_reason).reason_type = CdspStopReasonType::UnknownError;
                let b = msg.as_bytes();
                let len = b.len().min(255);
                std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, (*out_reason).message.as_mut_ptr(), len);
                (*out_reason).message[len] = 0;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn cdsp_set_fader_volume(engine: *mut CamillaEngine, fader: CdspFader, db: f32, _instant: bool) {
    if !engine.is_null() {
        let eng = unsafe { &*engine };
        eng.status_structs.processing.set_target_volume(fader_to_rust_index(fader), db);
    }
}

#[no_mangle]
pub extern "C" fn cdsp_set_fader_mute(engine: *mut CamillaEngine, fader: CdspFader, mute: bool) {
    if !engine.is_null() {
        let eng = unsafe { &*engine };
        eng.status_structs.processing.set_mute(fader_to_rust_index(fader), mute);
    }
}

#[no_mangle]
pub extern "C" fn cdsp_get_fader_volume(_engine: *const CamillaEngine, _fader: CdspFader) -> f32 {
    0.0
}

#[no_mangle]
pub extern "C" fn cdsp_get_fader_mute(_engine: *const CamillaEngine, _fader: CdspFader) -> bool {
    false
}

#[no_mangle]
pub extern "C" fn cdsp_get_vu_levels(engine: *const CamillaEngine, out_vu: *mut CdspVuLevels) -> bool {
    if engine.is_null() || out_vu.is_null() {
        return false;
    }
    let eng = unsafe { &*engine };
    let pb = eng.status_structs.playback.read();
    let cap = eng.status_structs.capture.read();

    let pb_rms = pb.signal_rms.last_sqrt();
    let pb_peak = pb.signal_peak.last();
    let cap_rms = cap.signal_rms.last_sqrt();
    let cap_peak = cap.signal_peak.last();

    let pb_ch = pb_rms.as_ref().map(|r| r.values.len()).unwrap_or(0);
    let cap_ch = cap_rms.as_ref().map(|r| r.values.len()).unwrap_or(0);

    unsafe {
        if (*out_vu).playback_rms.is_null() && (*out_vu).capture_rms.is_null() {
            (*out_vu).playback_channels = pb_ch;
            (*out_vu).capture_channels = cap_ch;
            return true;
        }

        if let Some(r) = pb_rms {
            if !(*out_vu).playback_rms.is_null() {
                for (i, &v) in r.values.iter().enumerate() {
                    *(*out_vu).playback_rms.add(i) = camillalib::utils::decibels::linear_to_db(v);
                }
            }
        }

        if let Some(p) = pb_peak {
            if !(*out_vu).playback_peak.is_null() {
                for (i, &v) in p.values.iter().enumerate() {
                    *(*out_vu).playback_peak.add(i) = camillalib::utils::decibels::linear_to_db(v);
                }
            }
        }

        if let Some(r) = cap_rms {
            if !(*out_vu).capture_rms.is_null() {
                for (i, &v) in r.values.iter().enumerate() {
                    *(*out_vu).capture_rms.add(i) = camillalib::utils::decibels::linear_to_db(v);
                }
            }
        }

        if let Some(p) = cap_peak {
            if !(*out_vu).capture_peak.is_null() {
                for (i, &v) in p.values.iter().enumerate() {
                    *(*out_vu).capture_peak.add(i) = camillalib::utils::decibels::linear_to_db(v);
                }
            }
        }

        (*out_vu).playback_channels = pb_ch;
        (*out_vu).capture_channels = cap_ch;
    }

    true
}

#[no_mangle]
pub extern "C" fn cdsp_get_spectrum(
    engine: *mut CamillaEngine,
    side: CdspSpectrumSide,
    channel: *const usize,
    min_freq: f32,
    max_freq: f32,
    n_bins: usize,
    out_spec: *mut CdspSpectrum,
) -> bool {
    if engine.is_null() || out_spec.is_null() || n_bins == 0 {
        return false;
    }
    camillalib::spectrum::request_spectrum_data();

    let eng = unsafe { &*engine };
    let is_capture = side == CdspSpectrumSide::Capture;
    let samplerate_usize: usize = eng
        .active_config
        .lock()
        .as_ref()
        .map(|c| {
            if is_capture {
                c.devices
                    .capture_samplerate
                    .map(|sr| sr.get())
                    .unwrap_or_else(|| c.devices.samplerate.get())
            } else {
                c.devices.samplerate.get()
            }
        })
        .unwrap_or(0);
    if samplerate_usize == 0 {
        return false;
    }

    let ch = if channel.is_null() { None } else { Some(unsafe { *channel }) };

    let res = if is_capture {
        let cap = eng.status_structs.capture.read();
        camillalib::spectrum::compute_spectrum(
            &cap.audio_buffer,
            min_freq as f64,
            max_freq as f64,
            n_bins,
            ch,
            samplerate_usize,
        )
    } else {
        let pb = eng.status_structs.playback.read();
        camillalib::spectrum::compute_spectrum(
            &pb.audio_buffer,
            min_freq as f64,
            max_freq as f64,
            n_bins,
            ch,
            samplerate_usize,
        )
    };

    match res {
        Ok(data) => unsafe {
            let count = data.frequencies.len().min(n_bins);
            if !(*out_spec).frequencies.is_null() {
                std::ptr::copy_nonoverlapping(data.frequencies.as_ptr(), (*out_spec).frequencies, count);
            }
            if !(*out_spec).magnitudes.is_null() {
                std::ptr::copy_nonoverlapping(data.magnitudes.as_ptr(), (*out_spec).magnitudes, count);
            }
            (*out_spec).count = count;
            true
        },
        Err(e) => unsafe {
            let err_str = format!("{:?}", e);
            let b = err_str.as_bytes();
            let len = b.len().min(127);
            std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, (*out_spec).error_message.as_mut_ptr(), len);
            (*out_spec).error_message[len] = 0;
            false
        },
    }
}

#[no_mangle]
pub extern "C" fn cdsp_get_samples(
    engine: *mut CamillaEngine,
    is_capture: bool,
    n_frames: usize,
    out_samples: *mut CdspAudioSamples,
    out_err: *mut CdspBackendError,
) -> bool {
    if engine.is_null() || out_samples.is_null() {
        return false;
    }
    camillalib::spectrum::request_spectrum_data();

    let eng = unsafe { &*engine };

    let configured_channels = eng
        .active_config
        .lock()
        .as_ref()
        .map(|c| {
            if is_capture {
                c.devices.capture.channels()
            } else {
                c.devices.playback.channels()
            }
        })
        .unwrap_or(0);

    let buffer_channels = if is_capture {
        let cap = eng.status_structs.capture.read();
        cap.audio_buffer.channel_count()
    } else {
        let pb = eng.status_structs.playback.read();
        pb.audio_buffer.channel_count()
    };

    let ch_count = if buffer_channels > 0 {
        buffer_channels
    } else if configured_channels > 0 {
        configured_channels
    } else {
        2
    };

    unsafe {
        if (*out_samples).channels.is_null() {
            (*out_samples).channels_count = ch_count;
            (*out_samples).frames = 0;
            return true;
        }

        let caller_channels = (*out_samples).channels_count;
        let effective_ch_count = if caller_channels > 0 {
            caller_channels.min(ch_count)
        } else {
            ch_count
        };

        if n_frames == 0 {
            (*out_samples).channels_count = effective_ch_count;
            (*out_samples).frames = 0;
            return true;
        }

        let mut max_copied_frames = 0;
        let mut any_success = false;

        for ch in 0..effective_ch_count {
            let channel_ptr = *(*out_samples).channels.add(ch);
            if channel_ptr.is_null() {
                continue;
            }

            let maybe_samples = if is_capture {
                let cap = eng.status_structs.capture.read();
                cap.audio_buffer.read_latest(n_frames, Some(ch))
            } else {
                let pb = eng.status_structs.playback.read();
                pb.audio_buffer.read_latest(n_frames, Some(ch))
            };

            if let Some(samples) = maybe_samples {
                let frames_to_copy = samples.len().min(n_frames);
                std::ptr::copy_nonoverlapping(samples.as_ptr(), channel_ptr, frames_to_copy);
                max_copied_frames = max_copied_frames.max(frames_to_copy);
                any_success = true;
            }
        }

        if !any_success {
            if !out_err.is_null() {
                (*out_err).error_type = CdspBackendErrorType::Unknown;
                let msg = b"Insufficient data or buffer empty\0";
                let len = msg.len().min(255);
                std::ptr::copy_nonoverlapping(msg.as_ptr() as *const c_char, (*out_err).message.as_mut_ptr(), len);
            }
            return false;
        }

        (*out_samples).channels_count = effective_ch_count;
        (*out_samples).frames = max_copied_frames;
        true
    }
}

#[no_mangle]
pub extern "C" fn cdsp_get_available_devices(
    backend: *const c_char,
    is_input: bool,
    out_devices: *mut *mut CdspDeviceInfo,
    out_count: *mut usize,
) -> bool {
    if backend.is_null() || out_devices.is_null() || out_count.is_null() {
        return false;
    }
    let backend_str = unsafe { CStr::from_ptr(backend).to_string_lossy() };
    let devices = camillalib::list_available_devices(&backend_str, is_input);

    let count = devices.len();
    if count == 0 {
        unsafe {
            *out_devices = std::ptr::null_mut();
            *out_count = 0;
        }
        return true;
    }

    unsafe {
        let size = count * std::mem::size_of::<CdspDeviceInfo>();
        let mem = libc::malloc(size) as *mut CdspDeviceInfo;
        if mem.is_null() {
            *out_devices = std::ptr::null_mut();
            *out_count = 0;
            return false;
        }

        for (i, (id, name)) in devices.into_iter().enumerate() {
            let item = mem.add(i);
            std::ptr::write_bytes(item, 0, 1);

            let id_bytes = id.as_bytes();
            let id_len = id_bytes.len().min(255);
            std::ptr::copy_nonoverlapping(id_bytes.as_ptr() as *const c_char, (*item).identifier.as_mut_ptr(), id_len);
            (*item).identifier[id_len] = 0;

            let n_bytes = name.as_bytes();
            let n_len = n_bytes.len().min(255);
            std::ptr::copy_nonoverlapping(n_bytes.as_ptr() as *const c_char, (*item).name.as_mut_ptr(), n_len);
            (*item).name[n_len] = 0;
            (*item).has_name = !name.is_empty();
        }

        *out_devices = mem;
        *out_count = count;
    }

    true
}

#[no_mangle]
pub extern "C" fn cdsp_get_device_capabilities(
    backend: *const c_char,
    device: *const c_char,
    is_capture: bool,
    out_desc: *mut *mut CdspDeviceDescriptor,
    out_err: *mut CdspDeviceError,
) -> bool {
    if backend.is_null() || device.is_null() || out_desc.is_null() {
        return false;
    }
    let backend_str = unsafe { CStr::from_ptr(backend).to_string_lossy() };
    let device_str = unsafe { CStr::from_ptr(device).to_string_lossy() };

    match camillalib::get_device_capabilities(&backend_str, &device_str, is_capture) {
        Ok(caps) => {
            unsafe {
                let desc_ptr = libc::malloc(std::mem::size_of::<CdspDeviceDescriptor>()) as *mut CdspDeviceDescriptor;
                if desc_ptr.is_null() {
                    return false;
                }
                std::ptr::write_bytes(desc_ptr, 0, 1);

                let d_bytes = caps.name.as_bytes();
                let d_len = d_bytes.len().min(255);
                std::ptr::copy_nonoverlapping(d_bytes.as_ptr() as *const c_char, (*desc_ptr).name.as_mut_ptr(), d_len);
                (*desc_ptr).name[d_len] = 0;

                let desc_bytes = caps.description.as_bytes();
                let desc_len = desc_bytes.len().min(255);
                std::ptr::copy_nonoverlapping(desc_bytes.as_ptr() as *const c_char, (*desc_ptr).description.as_mut_ptr(), desc_len);
                (*desc_ptr).description[desc_len] = 0;

                let sets_count = caps.capability_sets.len();
                if sets_count > 0 {
                    let set_ptr = libc::malloc(sets_count * std::mem::size_of::<CdspDeviceCapabilitySet>()) as *mut CdspDeviceCapabilitySet;
                    if !set_ptr.is_null() {
                        for (i, cap_set) in caps.capability_sets.into_iter().enumerate() {
                            let s_item = set_ptr.add(i);
                            std::ptr::write_bytes(s_item, 0, 1);

                            let m_str = format!("{:?}", cap_set.mode);
                            let m_bytes = m_str.as_bytes();
                            let m_len = m_bytes.len().min(63);
                            std::ptr::copy_nonoverlapping(m_bytes.as_ptr() as *const c_char, (*s_item).mode.as_mut_ptr(), m_len);
                            (*s_item).mode[m_len] = 0;

                            let ch_caps_count = cap_set.capabilities.len();
                            if ch_caps_count > 0 {
                                let ch_ptr = libc::malloc(ch_caps_count * std::mem::size_of::<CdspChannelCapability>()) as *mut CdspChannelCapability;
                                if !ch_ptr.is_null() {
                                    for (j, ch_cap) in cap_set.capabilities.into_iter().enumerate() {
                                        let c_item = ch_ptr.add(j);
                                        std::ptr::write_bytes(c_item, 0, 1);
                                        (*c_item).channels = ch_cap.channels as c_int;

                                        let sr_count = ch_cap.samplerates.len();
                                        if sr_count > 0 {
                                            let sr_ptr = libc::malloc(sr_count * std::mem::size_of::<CdspSamplerateCapability>()) as *mut CdspSamplerateCapability;
                                            if !sr_ptr.is_null() {
                                                for (k, sr_cap) in ch_cap.samplerates.into_iter().enumerate() {
                                                    let sr_item = sr_ptr.add(k);
                                                    std::ptr::write_bytes(sr_item, 0, 1);
                                                    (*sr_item).samplerate = sr_cap.samplerate as c_int;

                                                    let fmt_count = sr_cap.formats.len();
                                                    if fmt_count > 0 {
                                                        let fmt_arr = libc::malloc(fmt_count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
                                                        if !fmt_arr.is_null() {
                                                            for (m, fmt) in sr_cap.formats.into_iter().enumerate() {
                                                                let c_fmt = std::ffi::CString::new(fmt.as_str()).unwrap_or_default();
                                                                *fmt_arr.add(m) = libc::strdup(c_fmt.as_ptr());
                                                            }
                                                            (*sr_item).formats = fmt_arr;
                                                            (*sr_item).formats_count = fmt_count;
                                                        }
                                                    }
                                                }
                                                (*c_item).samplerates = sr_ptr;
                                                (*c_item).samplerates_count = sr_count;
                                            }
                                        }
                                    }
                                    (*s_item).capabilities = ch_ptr;
                                    (*s_item).capabilities_count = ch_caps_count;
                                }
                            }
                        }
                        (*desc_ptr).capability_sets = set_ptr;
                        (*desc_ptr).capability_sets_count = sets_count;
                    }
                }

                *out_desc = desc_ptr;
                true
            }
        }
        Err(e) => {
            if !out_err.is_null() {
                unsafe {
                    (*out_err).error_type = CdspDeviceErrorType::Unknown;
                    let err_str = format!("{:?}", e);
                    let err_bytes = err_str.into_bytes();
                    let len = err_bytes.len().min(255);
                    std::ptr::copy_nonoverlapping(err_bytes.as_ptr() as *const c_char, (*out_err).message.as_mut_ptr(), len);
                    (*out_err).message[len] = 0;
                }
            }
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn cdsp_free_device_capabilities(desc: *mut CdspDeviceDescriptor) {
    if desc.is_null() {
        return;
    }
    unsafe {
        if !(*desc).capability_sets.is_null() {
            for i in 0..(*desc).capability_sets_count {
                let set = (*desc).capability_sets.add(i);
                if !(*set).capabilities.is_null() {
                    for j in 0..(*set).capabilities_count {
                        let ch = (*set).capabilities.add(j);
                        if !(*ch).samplerates.is_null() {
                            for k in 0..(*ch).samplerates_count {
                                let sr = (*ch).samplerates.add(k);
                                if !(*sr).formats.is_null() {
                                    for m in 0..(*sr).formats_count {
                                        let fmt = *(*sr).formats.add(m);
                                        if !fmt.is_null() {
                                            libc::free(fmt as *mut c_void);
                                        }
                                    }
                                    libc::free((*sr).formats as *mut c_void);
                                }
                            }
                            libc::free((*ch).samplerates as *mut c_void);
                        }
                    }
                    libc::free((*set).capabilities as *mut c_void);
                }
            }
            libc::free((*desc).capability_sets as *mut c_void);
        }
        libc::free(desc as *mut c_void);
    }
}

