#include "engine/CDSPEngine.h"

#if defined(ENABLE_WEBAUDIO)
#include "cdsp_wasm.h"
#else
#include "cdsp/cdsp.h"
#endif

#include <cstring>  // for memset
#include <mutex>    // for mutex, lock_guard
#include <stdlib.h> // for free
#include <utility>  // for move

static CDSPEngine::LogCallback s_logCallback = nullptr;
static std::mutex s_logMutex;

#if defined(ENABLE_WEBAUDIO)
#include "cdsp_wasm.h"

#include <cmath>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

EM_JS(int, js_cdsp_is_captured, (), {
    if (typeof window !== 'undefined' && window.cdspBridge) {
        return window.cdspBridge.isCaptured ? 1 : 0;
    }
    return 0;
});

EM_JS(void, js_cdsp_start_capture, (), {
    if (typeof window !== 'undefined' && window.cdspBridge && typeof window.cdspBridge.startCapture === 'function') {
        window.cdspBridge.startCapture();
    }
});

EM_JS(void, js_cdsp_stop_capture, (), {
    if (typeof window !== 'undefined' && window.cdspBridge && typeof window.cdspBridge.stopCapture === 'function') {
        window.cdspBridge.stopCapture();
    }
});

EM_JS(void, js_cdsp_set_config, (const char* jsonStr), {
    if (typeof window !== 'undefined' && window.cdspBridge) {
        var str = UTF8ToString(jsonStr);
        window.cdspBridge.setConfig(str);
    }
});

EM_JS(void, js_cdsp_set_fader_volume, (int fader, double db, int instant), {
    if (typeof window !== 'undefined' && window.cdspBridge) {
        window.cdspBridge.setFaderVolume(fader, db, !!instant);
    }
});

EM_JS(void, js_cdsp_set_fader_mute, (int fader, int mute), {
    if (typeof window !== 'undefined' && window.cdspBridge) {
        window.cdspBridge.setFaderMute(fader, !!mute);
    }
});

EM_JS(void, js_cdsp_get_vu_levels, (float* inPeak, float* inRms, float* outPeak, float* outRms), {
    if (typeof window !== 'undefined' && window.cdspBridge && window.cdspBridge.telemetry) {
        var t = window.cdspBridge.telemetry;
        if (t.inPeak) {
            HEAPF32[inPeak >> 2] = (t.inPeak[0] !== undefined && Number.isFinite(t.inPeak[0])) ? t.inPeak[0] : -120.0;
            HEAPF32[(inPeak >> 2) + 1] = (t.inPeak[1] !== undefined && Number.isFinite(t.inPeak[1])) ? t.inPeak[1] : -120.0;
        }
        if (t.inRms) {
            HEAPF32[inRms >> 2] = (t.inRms[0] !== undefined && Number.isFinite(t.inRms[0])) ? t.inRms[0] : -120.0;
            HEAPF32[(inRms >> 2) + 1] = (t.inRms[1] !== undefined && Number.isFinite(t.inRms[1])) ? t.inRms[1] : -120.0;
        }
        if (t.outPeak) {
            HEAPF32[outPeak >> 2] = (t.outPeak[0] !== undefined && Number.isFinite(t.outPeak[0])) ? t.outPeak[0] : -120.0;
            HEAPF32[(outPeak >> 2) + 1] = (t.outPeak[1] !== undefined && Number.isFinite(t.outPeak[1])) ? t.outPeak[1] : -120.0;
        }
        if (t.outRms) {
            HEAPF32[outRms >> 2] = (t.outRms[0] !== undefined && Number.isFinite(t.outRms[0])) ? t.outRms[0] : -120.0;
            HEAPF32[(outRms >> 2) + 1] = (t.outRms[1] !== undefined && Number.isFinite(t.outRms[1])) ? t.outRms[1] : -120.0;
        }
    }
});

EM_JS(int, js_cdsp_get_spectrum, (int isCapture, float* outMag, int nBins), {
    if (typeof window !== 'undefined' && window.cdspBridge && window.cdspBridge.telemetry) {
        var t = window.cdspBridge.telemetry;
        var spec = isCapture ? t.inSpectrum : t.outSpectrum;
        if (spec && spec.length > 0) {
            var srcLen = spec.length;
            for (var i = 0; i < nBins; i++) {
                var srcIdx = (nBins === 1) ? 0 : Math.min(srcLen - 1, Math.floor((i / (nBins - 1)) * (srcLen - 1)));
                var val = spec[srcIdx];
                HEAPF32[(outMag >> 2) + i] = (val !== null && val !== undefined && Number.isFinite(val)) ? val : -120.0;
            }
            return 1;
        }
    }
    return 0;
});

EM_JS(int, js_cdsp_get_samples, (int isCapture, float* outLeft, float* outRight, int maxFrames), {
    if (typeof window !== 'undefined' && window.cdspBridge && window.cdspBridge.telemetry) {
        var t = window.cdspBridge.telemetry;
        var sL = isCapture ? t.inSamplesL : t.outSamplesL;
        var sR = isCapture ? t.inSamplesR : t.outSamplesR;
        if (sL && sR && sL.length > 0 && sR.length > 0) {
            var n = Math.min(maxFrames, Math.min(sL.length, sR.length));
            for (var i = 0; i < n; i++) {
                HEAPF32[(outLeft >> 2) + i] = Number.isFinite(sL[i]) ? sL[i] : 0.0;
                HEAPF32[(outRight >> 2) + i] = Number.isFinite(sR[i]) ? sR[i] : 0.0;
            }
            return n;
        }
    }
    return 0;
});
#endif

CDSPEngine::CDSPEngine() {
    m_engine = nullptr;
    m_isRunning = false;
}

CDSPEngine::~CDSPEngine() {
    if (m_engine) {
        cdsp_wasm_destroy(m_engine);
        m_engine = nullptr;
    }
}

namespace {
int faderToWasmIndex(Fader fader) {
    switch (fader) {
    case Fader::Main:
        return 0;
    case Fader::Aux1:
        return 1;
    case Fader::Aux2:
        return 2;
    case Fader::Aux3:
        return 3;
    case Fader::Aux4:
        return 4;
    }
    return 0;
}
} // namespace

bool CDSPEngine::start(const std::string& configJson, std::string& errorMessage) {
    errorMessage.clear();
    m_lastConfigJson = configJson;
    if (m_engine) {
        cdsp_wasm_destroy(m_engine);
        m_engine = nullptr;
    }
    m_engine = cdsp_wasm_create(configJson.c_str(), 48000, 128);

    m_isRunning = true;
#if defined(__EMSCRIPTEN__)
    js_cdsp_set_config(configJson.c_str());
    if (!js_cdsp_is_captured()) {
        js_cdsp_start_capture();
    }
#endif
    return true;
}

bool CDSPEngine::setConfig(const std::string& configJson, std::string& errorMessage) {
    return start(configJson, errorMessage);
}

void CDSPEngine::stop() {
    m_isRunning = false;
#if defined(__EMSCRIPTEN__)
    js_cdsp_stop_capture();
#endif
}

void CDSPEngine::poll() {
    // Synchronous WASM engine operates on-demand without thread polling
}

void CDSPEngine::setFaderVolume(Fader fader, float db, bool instant) {
    if (m_engine) {
        cdsp_wasm_set_fader_volume(m_engine, faderToWasmIndex(fader), static_cast<double>(db), instant);
    }
#if defined(__EMSCRIPTEN__)
    js_cdsp_set_fader_volume(faderToWasmIndex(fader), static_cast<double>(db), instant ? 1 : 0);
#endif
}

void CDSPEngine::setFaderMute(Fader fader, bool mute) {
    if (m_engine) {
        cdsp_wasm_set_fader_mute(m_engine, faderToWasmIndex(fader), mute);
    }
#if defined(__EMSCRIPTEN__)
    js_cdsp_set_fader_mute(faderToWasmIndex(fader), mute ? 1 : 0);
#endif
}

float CDSPEngine::getFaderVolume(Fader fader) const {
    if (m_engine) {
        return static_cast<float>(cdsp_wasm_get_fader_volume(m_engine, faderToWasmIndex(fader)));
    }
    return 0.0f;
}

bool CDSPEngine::isFaderMuted(Fader fader) const {
    if (m_engine) {
        return cdsp_wasm_get_fader_mute(m_engine, faderToWasmIndex(fader));
    }
    return false;
}

StateUpdate CDSPEngine::getStatus() const {
    StateUpdate res;
#if defined(__EMSCRIPTEN__)
    bool captured = js_cdsp_is_captured() != 0;
    res.state = (m_isRunning || captured) ? ProcessingState::Running : ProcessingState::Inactive;
#else
    res.state = m_isRunning ? ProcessingState::Running : ProcessingState::Inactive;
#endif
    res.stopReason.type = StopReasonType::None;
    return res;
}

VuLevels CDSPEngine::getVuLevels() const {
    VuLevels res;
    res.capture_peak.resize(2, -120.0f);
    res.capture_rms.resize(2, -120.0f);
    res.playback_peak.resize(2, -120.0f);
    res.playback_rms.resize(2, -120.0f);

#if defined(__EMSCRIPTEN__)
    js_cdsp_get_vu_levels(res.capture_peak.data(), res.capture_rms.data(),
                          res.playback_peak.data(), res.playback_rms.data());
#elif defined(ENABLE_WEBAUDIO)
    if (m_engine) {
        cdsp_wasm_get_vu_levels(m_engine, res.capture_peak.data(), res.capture_rms.data(),
                                res.playback_peak.data(), res.playback_rms.data());
    }
#endif
    return res;
}

bool CDSPEngine::getSpectrum(bool isCapture, int channel, double minFreq, double maxFreq, size_t nBins,
                             SpectrumData& outSpectrum) const {
    (void)channel;
    if (nBins == 0)
        return false;

    outSpectrum.frequencies.resize(nBins);
    outSpectrum.magnitudes.resize(nBins);

    // Compute logarithmic bin frequencies for visualizer
    double logMin = (minFreq > 0.0) ? std::log10(minFreq) : std::log10(20.0);
    double logMax = (maxFreq > minFreq) ? std::log10(maxFreq) : std::log10(20000.0);
    double step = (nBins > 1) ? (logMax - logMin) / static_cast<double>(nBins - 1) : 0.0;
    for (size_t i = 0; i < nBins; ++i) {
        outSpectrum.frequencies[i] = static_cast<float>(std::pow(10.0, logMin + static_cast<double>(i) * step));
    }

#if defined(__EMSCRIPTEN__)
    int ok = js_cdsp_get_spectrum(isCapture ? 1 : 0, outSpectrum.magnitudes.data(), static_cast<int>(nBins));
    if (!ok) {
        std::fill(outSpectrum.magnitudes.begin(), outSpectrum.magnitudes.end(), -120.0f);
    }
    return true;
#else
    if (!m_engine)
        return false;
    return cdsp_wasm_get_spectrum(m_engine, isCapture, channel >= 0 ? channel : 0, minFreq, maxFreq, nBins,
                                  outSpectrum.magnitudes.data());
#endif
}

bool CDSPEngine::getSamples(bool isCapture, size_t nFrames, AudioSamplesData& outSamples) const {
    if (nFrames == 0) {
        outSamples.channels.clear();
        return false;
    }
    outSamples.channels.resize(2);
    outSamples.channels[0].resize(nFrames);
    outSamples.channels[1].resize(nFrames);

#if defined(__EMSCRIPTEN__)
    int n = js_cdsp_get_samples(isCapture ? 1 : 0, outSamples.channels[0].data(), outSamples.channels[1].data(), static_cast<int>(nFrames));
    if (n <= 0) {
        outSamples.channels.clear();
        return false;
    }
    if (static_cast<size_t>(n) < nFrames) {
        outSamples.channels[0].resize(n);
        outSamples.channels[1].resize(n);
    }
    return true;
#elif defined(ENABLE_WEBAUDIO)
    if (!m_engine) {
        outSamples.channels.clear();
        return false;
    }
    size_t n = cdsp_wasm_get_samples(m_engine, isCapture, nFrames, outSamples.channels[0].data(), outSamples.channels[1].data());
    if (n == 0) {
        outSamples.channels.clear();
        return false;
    }
    if (n < nFrames) {
        outSamples.channels[0].resize(n);
        outSamples.channels[1].resize(n);
    }
    return true;
#else
    (void)isCapture;
    (void)nFrames;
    outSamples.channels.clear();
    return false;
#endif
}

std::vector<AudioDevice> CDSPEngine::getAvailableDevices(const std::string& backend, bool input) const {
    (void)backend;
    std::vector<AudioDevice> result;
    if (input) {
        result.push_back(AudioDevice{"default", "WebAudio System Input"});
        result.push_back(AudioDevice{"tab", "Active Tab Audio Stream"});
    } else {
        result.push_back(AudioDevice{"default", "WebAudio Default Output"});
    }
    return result;
}

std::optional<AudioDeviceDescriptor>
CDSPEngine::getDeviceCapabilities(const std::string& backend, const std::string& device, bool isCapture) const {
    (void)backend;
    (void)device;
    (void)isCapture;
    AudioDeviceDescriptor desc;
    desc.name = "WebAudio";
    DeviceCapabilitySet capSet;
    capSet.mode = "Standard";
    ChannelCapability chCap;
    chCap.channels = 2;
    for (int rate : {44100, 48000, 88200, 96000}) {
        SamplerateCapability srCap;
        srCap.samplerate = rate;
        srCap.formats.push_back("F32");
        chCap.samplerates.push_back(srCap);
    }
    capSet.capabilities.push_back(chCap);
    desc.capability_sets.push_back(capSet);
    return desc;
}

void CDSPEngine::setLogLevel(const std::string& levelStr) {
    (void)levelStr;
}

void CDSPEngine::setLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    s_logCallback = std::move(callback);
}

#else

CDSPEngine::CDSPEngine() {
    m_engine = cdsp_engine_create();
}

CDSPEngine::~CDSPEngine() {
    if (m_engine) {
        cdsp_engine_free(m_engine);
        m_engine = nullptr;
    }
}

namespace {
cdsp_fader_t faderToCFader(Fader fader) {
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
} // namespace

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

    cdsp_spectrum_t res{};
    res.frequencies = outSpectrum.frequencies.data();
    res.magnitudes = outSpectrum.magnitudes.data();

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

#endif
