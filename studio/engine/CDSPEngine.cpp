#include "engine/CDSPEngine.h"

#include <cstring>
#include <iostream>

CDSPEngine::CDSPEngine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_engine = dsp_engine_create();
}

CDSPEngine::~CDSPEngine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        dsp_engine_free(m_engine);
        m_engine = nullptr;
    }
}

fader_t CDSPEngine::faderToCFader(Fader fader) {
    switch (fader) {
    case Fader::Main:
        return FADER_MAIN;
    case Fader::Aux1:
        return FADER_AUX1;
    case Fader::Aux2:
        return FADER_AUX2;
    case Fader::Aux3:
        return FADER_AUX3;
    case Fader::Aux4:
        return FADER_AUX4;
    }
    return FADER_MAIN;
}

bool CDSPEngine::start(const std::string& configJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine)
        return false;

    audio_backend_error_t err;
    memset(&err, 0, sizeof(err));
    bool success = dsp_engine_set_config(m_engine, configJson.c_str(), &err);
    if (!success) {
        errorMessage = err.message;
    }
    return success;
}

void CDSPEngine::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        dsp_engine_stop(m_engine);
    }
}

void CDSPEngine::setFaderVolume(Fader fader, float db, bool instant) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        dsp_engine_set_fader_volume(m_engine, faderToCFader(fader), db, instant);
    }
}

void CDSPEngine::setFaderMute(Fader fader, bool mute) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        dsp_engine_set_fader_mute(m_engine, faderToCFader(fader), mute);
    }
}

float CDSPEngine::getFaderVolume(Fader fader) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        return dsp_engine_get_fader_volume(m_engine, faderToCFader(fader));
    }
    return 0.0f;
}

bool CDSPEngine::isFaderMuted(Fader fader) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engine) {
        return dsp_engine_is_fader_muted(m_engine, faderToCFader(fader));
    }
    return false;
}

StateUpdate CDSPEngine::getStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    StateUpdate res;
    if (!m_engine)
        return res;

    state_update_t st = dsp_engine_get_status(m_engine);
    switch (st.state) {
    case PROCESSING_STATE_RUNNING:
        res.state = ProcessingState::Running;
        break;
    case PROCESSING_STATE_PAUSED:
        res.state = ProcessingState::Paused;
        break;
    case PROCESSING_STATE_INACTIVE:
        res.state = ProcessingState::Inactive;
        break;
    case PROCESSING_STATE_STARTING:
        res.state = ProcessingState::Starting;
        break;
    case PROCESSING_STATE_STALLED:
        res.state = ProcessingState::Stalled;
        break;
    default:
        res.state = ProcessingState::Inactive;
        break;
    }

    switch (st.stop_reason.type) {
    case STOP_REASON_NONE:
        res.stopReason.type = StopReasonType::None;
        break;
    case STOP_REASON_DONE:
        res.stopReason.type = StopReasonType::Done;
        break;
    case STOP_REASON_CAPTURE_ERROR:
        res.stopReason.type = StopReasonType::CaptureError;
        res.stopReason.message = st.stop_reason.message;
        break;
    case STOP_REASON_PLAYBACK_ERROR:
        res.stopReason.type = StopReasonType::PlaybackError;
        res.stopReason.message = st.stop_reason.message;
        break;
    case STOP_REASON_CAPTURE_FORMAT_CHANGE:
        res.stopReason.type = StopReasonType::CaptureFormatChange;
        res.stopReason.formatChangeRate = static_cast<int>(st.stop_reason.format_change_rate);
        break;
    case STOP_REASON_PLAYBACK_FORMAT_CHANGE:
        res.stopReason.type = StopReasonType::PlaybackFormatChange;
        res.stopReason.formatChangeRate = static_cast<int>(st.stop_reason.format_change_rate);
        break;
    case STOP_REASON_UNKNOWN_ERROR:
        res.stopReason.type = StopReasonType::UnknownError;
        res.stopReason.message = st.stop_reason.message;
        break;
    default:
        res.stopReason.type = StopReasonType::None;
        break;
    }

    return res;
}

VuLevels CDSPEngine::getVuLevels() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    VuLevels res;
    if (!m_engine)
        return res;

    vu_levels_t levels = dsp_engine_get_vu_levels(m_engine);
    for (size_t i = 0; i < levels.playback_channels; ++i) {
        res.playback_rms.push_back(static_cast<float>(levels.playback_rms[i]));
        res.playback_peak.push_back(static_cast<float>(levels.playback_peak[i]));
    }
    for (size_t i = 0; i < levels.capture_channels; ++i) {
        res.capture_rms.push_back(static_cast<float>(levels.capture_rms[i]));
        res.capture_peak.push_back(static_cast<float>(levels.capture_peak[i]));
    }

    dsp_engine_free_vu_levels(&levels);
    return res;
}

bool CDSPEngine::getSpectrum(bool isCapture, int channel, double minFreq, double maxFreq, size_t nBins,
                             SpectrumData& outSpectrum) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine)
        return false;

    spectrum_result_t res;
    memset(&res, 0, sizeof(res));
    spectrum_status_t st = dsp_engine_get_spectrum(m_engine, isCapture, channel, minFreq, maxFreq, nBins, &res);
    if (st != SPECTRUM_OK) {
        return false;
    }

    outSpectrum.frequencies.assign(res.frequencies, res.frequencies + res.count);
    outSpectrum.magnitudes.assign(res.magnitudes, res.magnitudes + res.count);
    return true;
}

bool CDSPEngine::getSamples(bool isCapture, size_t nFrames, AudioSamplesData& outSamples) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine)
        return false;

    audio_backend_error_t err;
    memset(&err, 0, sizeof(err));
    audio_samples_t* res = dsp_engine_get_samples(m_engine, isCapture, nFrames, &err);
    if (!res)
        return false;

    outSamples.channels.clear();
    for (size_t ch = 0; ch < res->channels_count; ++ch) {
        std::vector<float> chSamples;
        if (res->channels[ch]) {
            for (size_t f = 0; f < res->frames; ++f) {
                chSamples.push_back(static_cast<float>(res->channels[ch][f]));
            }
        }
        outSamples.channels.push_back(chSamples);
    }

    dsp_engine_free_samples(res);
    return true;
}

std::vector<AudioDevice> CDSPEngine::getAvailableDevices(const std::string& backend, bool input) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AudioDevice> result;
    if (!m_engine)
        return result;

    audio_device_t devs[32];
    memset(devs, 0, sizeof(devs));
    int count = dsp_engine_get_available_devices(backend.c_str(), input, devs, 32);
    if (count > 0) {
        for (int i = 0; i < count; ++i) {
            result.push_back(AudioDevice{devs[i].name});
        }
    }
    return result;
}

std::optional<AudioDeviceDescriptor>
CDSPEngine::getDeviceCapabilities(const std::string& backend, const std::string& device, bool isCapture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine)
        return std::nullopt;

    device_error_t devErr;
    memset(&devErr, 0, sizeof(devErr));
    audio_device_descriptor_t* desc =
        dsp_engine_get_device_capabilities(backend.c_str(), device.c_str(), isCapture, &devErr);
    if (!desc)
        return std::nullopt;

    AudioDeviceDescriptor res;
    res.name = desc->name;

    for (size_t i = 0; i < desc->capability_sets_count; ++i) {
        const auto& cSet = desc->capability_sets[i];
        DeviceCapabilitySet setRes;
        for (size_t j = 0; j < cSet.capabilities_count; ++j) {
            const auto& chCap = cSet.capabilities[j];
            ChannelCapability capRes;
            capRes.channels = chCap.channels;
            for (size_t k = 0; k < chCap.samplerates_count; ++k) {
                const auto& srCap = chCap.samplerates[k];
                SamplerateCapability srRes;
                srRes.samplerate = srCap.samplerate;
                for (size_t m = 0; m < srCap.formats_count; ++m) {
                    if (srCap.formats[m]) {
                        srRes.formats.push_back(srCap.formats[m]);
                    }
                }
                capRes.samplerates.push_back(srRes);
            }
            setRes.capabilities.push_back(capRes);
        }
        res.capability_sets.push_back(setRes);
    }

    dsp_engine_free_device_capabilities(desc);
    return res;
}

void CDSPEngine::setLogLevel(const std::string& levelStr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    dsp_engine_set_log_level(log_level_from_string(levelStr.c_str()));
}
