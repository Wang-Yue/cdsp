#include "engine/CDSPEngine.h"

#include <cstring>
#include <iostream>
#include <mutex>

static CDSPEngine::LogCallback s_logCallback = nullptr;
static std::mutex s_logMutex;

CDSPEngine::CDSPEngine() {
    m_engine = cdsp_engine_create();
}

CDSPEngine::~CDSPEngine() {
    if (m_engine) {
        cdsp_engine_free(m_engine);
        m_engine = nullptr;
    }
}

cdsp_fader_t CDSPEngine::faderToCFader(Fader fader) {
    switch (fader) {
    case Fader::Main:
        return CDSP_FADER_MAIN;
    case Fader::Aux1:
        return CDSP_FADER_AUX1;
    case Fader::Aux2:
        return CDSP_FADER_AUX2;
    case Fader::Aux3:
        return CDSP_FADER_AUX3;
    case Fader::Aux4:
        return CDSP_FADER_AUX4;
    }
    return CDSP_FADER_MAIN;
}

bool CDSPEngine::start(const std::string& configJson, std::string& errorMessage) {
    errorMessage.clear();
    if (!m_engine)
        return false;

    cdsp_backend_error_t err;
    memset(&err, 0, sizeof(err));
    bool success = cdsp_set_config_json(m_engine, configJson.c_str(), &err);
    if (!success) {
        std::string msg = err.message;
        if (err.type == CDSP_BACKEND_ERR_CONFIG_PARSE) {
            errorMessage = "Config parse error: " + msg;
        } else {
            errorMessage = "Command send error: " + msg;
        }
    }
    return success;
}

bool CDSPEngine::setConfig(const std::string& configJson, std::string& errorMessage) {
    return start(configJson, errorMessage);
}

void CDSPEngine::stop() {
    if (m_engine) {
        cdsp_stop(m_engine);
    }
}

void CDSPEngine::poll() {
    if (m_engine) {
        cdsp_engine_poll(m_engine);
    }
}

void CDSPEngine::setFaderVolume(Fader fader, float db, bool instant) {
    if (m_engine) {
        cdsp_set_fader_volume(m_engine, faderToCFader(fader), db, instant);
    }
}

void CDSPEngine::setFaderMute(Fader fader, bool mute) {
    if (m_engine) {
        cdsp_set_fader_mute(m_engine, faderToCFader(fader), mute);
    }
}

float CDSPEngine::getFaderVolume(Fader fader) const {
    if (m_engine) {
        return cdsp_get_fader_volume(m_engine, faderToCFader(fader));
    }
    return 0.0f;
}

bool CDSPEngine::isFaderMuted(Fader fader) const {
    if (m_engine) {
        return cdsp_get_fader_mute(m_engine, faderToCFader(fader));
    }
    return false;
}

StateUpdate CDSPEngine::getStatus() const {
    StateUpdate res;
    if (!m_engine)
        return res;

    cdsp_processing_state_t st = cdsp_get_state(m_engine);
    switch (st) {
    case CDSP_PROCESSING_STATE_RUNNING:
        res.state = ProcessingState::Running;
        break;
    case CDSP_PROCESSING_STATE_PAUSED:
        res.state = ProcessingState::Paused;
        break;
    case CDSP_PROCESSING_STATE_INACTIVE:
        res.state = ProcessingState::Inactive;
        break;
    case CDSP_PROCESSING_STATE_STARTING:
        res.state = ProcessingState::Starting;
        break;
    case CDSP_PROCESSING_STATE_STALLED:
        res.state = ProcessingState::Stalled;
        break;
    default:
        res.state = ProcessingState::Inactive;
        break;
    }

    cdsp_stop_reason_t stop_reason;
    cdsp_get_stop_reason(m_engine, &stop_reason);
    switch (stop_reason.type) {
    case CDSP_STOP_REASON_NONE:
        res.stopReason.type = StopReasonType::None;
        break;
    case CDSP_STOP_REASON_DONE:
        res.stopReason.type = StopReasonType::Done;
        break;
    case CDSP_STOP_REASON_CAPTURE_ERROR:
        res.stopReason.type = StopReasonType::CaptureError;
        res.stopReason.message = stop_reason.message;
        break;
    case CDSP_STOP_REASON_PLAYBACK_ERROR:
        res.stopReason.type = StopReasonType::PlaybackError;
        res.stopReason.message = stop_reason.message;
        break;
    case CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE:
        res.stopReason.type = StopReasonType::CaptureFormatChange;
        res.stopReason.formatChangeRate = static_cast<int>(stop_reason.format_change_rate);
        break;
    case CDSP_STOP_REASON_PLAYBACK_FORMAT_CHANGE:
        res.stopReason.type = StopReasonType::PlaybackFormatChange;
        res.stopReason.formatChangeRate = static_cast<int>(stop_reason.format_change_rate);
        break;
    case CDSP_STOP_REASON_UNKNOWN_ERROR:
        res.stopReason.type = StopReasonType::UnknownError;
        res.stopReason.message = stop_reason.message;
        break;
    default:
        res.stopReason.type = StopReasonType::None;
        break;
    }

    return res;
}

VuLevels CDSPEngine::getVuLevels() const {
    VuLevels res;
    if (!m_engine)
        return res;

    cdsp_vu_levels_t query = {};
    if (cdsp_get_vu_levels(m_engine, &query)) {
        size_t pb_ch = query.playback_channels;
        size_t cap_ch = query.capture_channels;
        res.playback_rms.resize(pb_ch);
        res.playback_peak.resize(pb_ch);
        res.capture_rms.resize(cap_ch);
        res.capture_peak.resize(cap_ch);

        cdsp_vu_levels_t levels = {pb_ch > 0 ? res.playback_rms.data() : nullptr,
                                   pb_ch > 0 ? res.playback_peak.data() : nullptr,
                                   cap_ch > 0 ? res.capture_rms.data() : nullptr,
                                   cap_ch > 0 ? res.capture_peak.data() : nullptr,
                                   0,
                                   0};
        if (cdsp_get_vu_levels(m_engine, &levels)) {
            return res;
        }
    }
    return res;
}

bool CDSPEngine::getSpectrum(bool isCapture, int channel, double minFreq, double maxFreq, size_t nBins,
                             SpectrumData& outSpectrum) const {
    if (!m_engine || nBins == 0)
        return false;

    cdsp_spectrum_side_t side = isCapture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK;
    size_t ch_val = channel >= 0 ? static_cast<size_t>(channel) : 0;
    const size_t* ch_ptr = channel >= 0 ? &ch_val : nullptr;

    outSpectrum.frequencies.resize(nBins);
    outSpectrum.magnitudes.resize(nBins);

    cdsp_spectrum_t res = {outSpectrum.frequencies.data(), outSpectrum.magnitudes.data(), 0};

    bool success = cdsp_get_spectrum(m_engine, side, ch_ptr, static_cast<float>(minFreq), static_cast<float>(maxFreq),
                                     nBins, &res);
    if (!success || res.count == 0) {
        outSpectrum.frequencies.clear();
        outSpectrum.magnitudes.clear();
        return false;
    }

    if (res.count < nBins) {
        outSpectrum.frequencies.resize(res.count);
        outSpectrum.magnitudes.resize(res.count);
    }
    return true;
}

bool CDSPEngine::getSamples(bool isCapture, size_t nFrames, AudioSamplesData& outSamples) const {
    if (!m_engine || nFrames == 0)
        return false;

    cdsp_backend_error_t err;
    memset(&err, 0, sizeof(err));

    cdsp_audio_samples_t query = {};
    if (!cdsp_get_samples(m_engine, isCapture, nFrames, &query, &err))
        return false;

    size_t ch_count = query.channels_count;
    if (ch_count == 0) {
        outSamples.channels.clear();
        return true;
    }

    outSamples.channels.resize(ch_count);
    std::vector<float*> chan_ptrs(ch_count);
    for (size_t ch = 0; ch < ch_count; ++ch) {
        outSamples.channels[ch].resize(nFrames);
        chan_ptrs[ch] = outSamples.channels[ch].data();
    }

    cdsp_audio_samples_t samples = {chan_ptrs.data(), 0, 0};

    if (!cdsp_get_samples(m_engine, isCapture, nFrames, &samples, &err)) {
        outSamples.channels.clear();
        return false;
    }

    if (samples.frames < nFrames) {
        for (size_t ch = 0; ch < ch_count; ++ch) {
            outSamples.channels[ch].resize(samples.frames);
        }
    }

    return true;
}

std::vector<AudioDevice> CDSPEngine::getAvailableDevices(const std::string& backend, bool input) const {
    std::vector<AudioDevice> result;

    cdsp_device_info_t* devs = nullptr;
    size_t count = 0;
    bool success = cdsp_get_available_devices(backend.c_str(), input, &devs, &count);
    if (success && count > 0 && devs) {
        for (size_t i = 0; i < count; ++i) {
            result.push_back(AudioDevice{devs[i].identifier, devs[i].name});
        }
    }
    if (devs) {
        free(devs);
    }
    return result;
}

std::optional<AudioDeviceDescriptor>
CDSPEngine::getDeviceCapabilities(const std::string& backend, const std::string& device, bool isCapture) const {
    cdsp_device_error_t devErr;
    memset(&devErr, 0, sizeof(devErr));
    cdsp_device_descriptor_t* desc = nullptr;
    bool success = cdsp_get_device_capabilities(backend.c_str(), device.c_str(), isCapture, &desc, &devErr);
    if (!success || !desc) {
        if (desc) {
            cdsp_free_device_capabilities(desc);
        }
        if (devErr.type != CDSP_DEVICE_ERROR_NONE) {
            std::lock_guard<std::mutex> lock(s_logMutex);
            if (s_logCallback) {
                s_logCallback("ERROR", "CDSPEngine", std::string("Device capabilities error: ") + devErr.message);
            }
        }
        return std::nullopt;
    }

    AudioDeviceDescriptor res;
    res.name = desc->name;

    if (desc->capability_sets) {
        for (size_t i = 0; i < desc->capability_sets_count; ++i) {
            const auto& cSet = desc->capability_sets[i];
            DeviceCapabilitySet setRes;
            setRes.mode = cSet.mode;
            if (cSet.capabilities) {
                for (size_t j = 0; j < cSet.capabilities_count; ++j) {
                    const auto& chCap = cSet.capabilities[j];
                    ChannelCapability capRes;
                    capRes.channels = chCap.channels;
                    if (chCap.samplerates) {
                        for (size_t k = 0; k < chCap.samplerates_count; ++k) {
                            const auto& srCap = chCap.samplerates[k];
                            SamplerateCapability srRes;
                            srRes.samplerate = srCap.samplerate;
                            if (srCap.formats) {
                                for (size_t m = 0; m < srCap.formats_count; ++m) {
                                    if (srCap.formats[m]) {
                                        srRes.formats.push_back(srCap.formats[m]);
                                    }
                                }
                            }
                            capRes.samplerates.push_back(srRes);
                        }
                    }
                    setRes.capabilities.push_back(capRes);
                }
            }
            res.capability_sets.push_back(setRes);
        }
    }

    cdsp_free_device_capabilities(desc);
    return res;
}

void CDSPEngine::setLogLevel(const std::string& levelStr) {
    cdsp_set_log_level(levelStr.c_str());
}

static void onCdspLogBridge(const char* level, const char* label, const char* message, void* user_data) {
    (void)user_data;
    CDSPEngine::LogCallback cb;
    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        cb = s_logCallback;
    }
    if (cb) {
        cb(level ? level : "", label ? label : "", message ? message : "");
    }
}

void CDSPEngine::setLogCallback(LogCallback callback) {
    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        s_logCallback = std::move(callback);
    }
    if (s_logCallback) {
        cdsp_set_log_callback(onCdspLogBridge, nullptr);
    } else {
        cdsp_set_log_callback(nullptr, nullptr);
    }
}
