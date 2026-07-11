#include "config/DSPConfigTypes.h"
#include <stdexcept>
#include <set>
#include <cmath>

std::string faderToString(Fader fader) {
    switch (fader) {
    case Fader::Main: return "Main";
    case Fader::Aux1: return "Aux1";
    case Fader::Aux2: return "Aux2";
    case Fader::Aux3: return "Aux3";
    case Fader::Aux4: return "Aux4";
    }
    return "Main";
}

Fader stringToFader(const std::string& str) {
    if (str == "Aux1" || str == "aux1") return Fader::Aux1;
    if (str == "Aux2" || str == "aux2") return Fader::Aux2;
    if (str == "Aux3" || str == "aux3") return Fader::Aux3;
    if (str == "Aux4" || str == "aux4") return Fader::Aux4;
    return Fader::Main;
}

std::string processingStateToString(ProcessingState state) {
    switch (state) {
    case ProcessingState::Inactive: return "Inactive";
    case ProcessingState::Starting: return "Starting";
    case ProcessingState::Running: return "Running";
    case ProcessingState::Paused: return "Paused";
    case ProcessingState::Stalled: return "Stalled";
    }
    return "Inactive";
}

ProcessingState uint8ToProcessingState(uint8_t rawByte) {
    switch (rawByte) {
    case 1: return ProcessingState::Starting;
    case 2: return ProcessingState::Running;
    case 3: return ProcessingState::Paused;
    case 4: return ProcessingState::Stalled;
    default: return ProcessingState::Inactive;
    }
}

std::string audioBackendTypeToString(AudioBackendType type) {
    switch (type) {
    case AudioBackendType::CoreAudio: return "CoreAudio";
    case AudioBackendType::RawFile: return "RawFile";
    case AudioBackendType::WavFile: return "WavFile";
    case AudioBackendType::SignalGenerator: return "SignalGenerator";
    }
    return "CoreAudio";
}

AudioBackendType stringToAudioBackendType(const std::string& str) {
    if (str == "RawFile" || str == "File") return AudioBackendType::RawFile;
    if (str == "WavFile") return AudioBackendType::WavFile;
    if (str == "SignalGenerator") return AudioBackendType::SignalGenerator;
    return AudioBackendType::CoreAudio;
}

std::string sdmFilterToString(SDMFilter f) {
    switch (f) {
    case SDMFilter::Clans4: return "clans-4";
    case SDMFilter::SDM4: return "sdm-4";
    case SDMFilter::Clans5: return "clans-5";
    case SDMFilter::SDM5: return "sdm-5";
    case SDMFilter::Clans6: return "clans-6";
    case SDMFilter::SDM6: return "sdm-6";
    case SDMFilter::Clans7: return "clans-7";
    case SDMFilter::SDM7: return "sdm-7";
    case SDMFilter::Clans8: return "clans-8";
    case SDMFilter::SDM8: return "sdm-8";
    }
    return "sdm-6";
}

SDMFilter stringToSDMFilter(const std::string& str) {
    if (str == "clans-4") return SDMFilter::Clans4;
    if (str == "sdm-4") return SDMFilter::SDM4;
    if (str == "clans-5") return SDMFilter::Clans5;
    if (str == "sdm-5") return SDMFilter::SDM5;
    if (str == "clans-6") return SDMFilter::Clans6;
    if (str == "sdm-6") return SDMFilter::SDM6;
    if (str == "clans-7") return SDMFilter::Clans7;
    if (str == "sdm-7") return SDMFilter::SDM7;
    if (str == "clans-8") return SDMFilter::Clans8;
    if (str == "sdm-8") return SDMFilter::SDM8;
    return SDMFilter::SDM6;
}

std::string delayUnitToString(DelayUnit unit) {
    switch (unit) {
    case DelayUnit::ms: return "ms";
    case DelayUnit::us: return "us";
    case DelayUnit::samples: return "samples";
    case DelayUnit::mm: return "mm";
    }
    return "ms";
}

DelayUnit stringToDelayUnit(const std::string& str) {
    if (str == "us") return DelayUnit::us;
    if (str == "samples") return DelayUnit::samples;
    if (str == "mm") return DelayUnit::mm;
    return DelayUnit::ms;
}

double delayUnitToSamples(DelayUnit unit, double delay, double sampleRate) {
    switch (unit) {
    case DelayUnit::ms: return delay / 1000.0 * sampleRate;
    case DelayUnit::us: return delay / 1000000.0 * sampleRate;
    case DelayUnit::samples: return delay;
    case DelayUnit::mm: return delay / 1000.0 * sampleRate / 343.0;
    }
    return delay / 1000.0 * sampleRate;
}

std::string gainScaleToString(GainScale s) {
    return s == GainScale::linear ? "linear" : "dB";
}

GainScale stringToGainScale(const std::string& str) {
    return str == "linear" ? GainScale::linear : GainScale::dB;
}

std::string resamplerTypeToString(ResamplerType t) {
    switch (t) {
    case ResamplerType::Synchronous: return "Synchronous";
    case ResamplerType::Apple: return "Apple";
    case ResamplerType::AsyncSinc: return "AsyncSinc";
    case ResamplerType::AsyncPoly: return "AsyncPoly";
    }
    return "Synchronous";
}

ResamplerType stringToResamplerType(const std::string& str) {
    if (str == "Apple") return ResamplerType::Apple;
    if (str == "AsyncSinc") return ResamplerType::AsyncSinc;
    if (str == "AsyncPoly") return ResamplerType::AsyncPoly;
    return ResamplerType::Synchronous;
}

std::string resamplerProfileToString(ResamplerProfile p) {
    switch (p) {
    case ResamplerProfile::VeryFast: return "VeryFast";
    case ResamplerProfile::Fast: return "Fast";
    case ResamplerProfile::Balanced: return "Balanced";
    case ResamplerProfile::Accurate: return "Accurate";
    }
    return "Balanced";
}

ResamplerProfile stringToResamplerProfile(const std::string& str) {
    if (str == "VeryFast") return ResamplerProfile::VeryFast;
    if (str == "Fast") return ResamplerProfile::Fast;
    if (str == "Accurate") return ResamplerProfile::Accurate;
    return ResamplerProfile::Balanced;
}

std::string filterTypeToString(FilterType t) {
    switch (t) {
    case FilterType::Gain: return "Gain";
    case FilterType::Volume: return "Volume";
    case FilterType::Loudness: return "Loudness";
    case FilterType::Biquad: return "Biquad";
    case FilterType::Conv: return "Conv";
    case FilterType::Delay: return "Delay";
    case FilterType::BiquadCombo: return "BiquadCombo";
    case FilterType::DiffEq: return "DiffEq";
    case FilterType::Dither: return "Dither";
    case FilterType::Limiter: return "Limiter";
    case FilterType::LookaheadLimiter: return "LookaheadLimiter";
    }
    return "Gain";
}

std::string biquadComboTypeToString(BiquadComboType t) {
    switch (t) {
    case BiquadComboType::ButterworthHighpass: return "ButterworthHighpass";
    case BiquadComboType::ButterworthLowpass: return "ButterworthLowpass";
    case BiquadComboType::LinkwitzRileyHighpass: return "LinkwitzRileyHighpass";
    case BiquadComboType::LinkwitzRileyLowpass: return "LinkwitzRileyLowpass";
    case BiquadComboType::Tilt: return "Tilt";
    case BiquadComboType::FivePointPeq: return "FivePointPeq";
    case BiquadComboType::GraphicEqualizer: return "GraphicEqualizer";
    }
    return "ButterworthLowpass";
}

std::string ditherTypeToString(DitherType t) {
    switch (t) {
    case DitherType::None: return "None";
    case DitherType::Flat: return "Flat";
    case DitherType::Highpass: return "Highpass";
    case DitherType::Fweighted441: return "Fweighted441";
    case DitherType::FweightedLong441: return "FweightedLong441";
    case DitherType::FweightedShort441: return "FweightedShort441";
    case DitherType::Gesemann441: return "Gesemann441";
    case DitherType::Gesemann48: return "Gesemann48";
    case DitherType::Lipshitz441: return "Lipshitz441";
    case DitherType::LipshitzLong441: return "LipshitzLong441";
    case DitherType::Shibata441: return "Shibata441";
    case DitherType::ShibataHigh441: return "ShibataHigh441";
    case DitherType::ShibataLow441: return "ShibataLow441";
    case DitherType::Shibata48: return "Shibata48";
    case DitherType::ShibataHigh48: return "ShibataHigh48";
    case DitherType::ShibataLow48: return "ShibataLow48";
    case DitherType::Shibata882: return "Shibata882";
    case DitherType::ShibataLow882: return "ShibataLow882";
    case DitherType::Shibata96: return "Shibata96";
    case DitherType::ShibataLow96: return "ShibataLow96";
    case DitherType::Shibata192: return "Shibata192";
    case DitherType::ShibataLow192: return "ShibataLow192";
    }
    return "Flat";
}

std::string processorTypeToString(ProcessorType t) {
    switch (t) {
    case ProcessorType::Compressor: return "Compressor";
    case ProcessorType::NoiseGate: return "NoiseGate";
    case ProcessorType::RACE: return "RACE";
    }
    return "Compressor";
}

// JSON Encoders

QJsonObject GeneratorConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(type);
    if (freq.has_value()) obj["freq"] = freq.value();
    obj["level"] = level;
    return obj;
}

QJsonObject CoreAudioCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "CoreAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty()) obj["device"] = QString::fromStdString(device.value());
    if (format.has_value()) obj["format"] = QString::fromStdString(format.value());
    if (bypassDoP.has_value()) obj["bypass_dop"] = bypassDoP.value();
    if (dopCutoffHz.has_value()) obj["dop_cutoff_hz"] = dopCutoffHz.value();
    return obj;
}

QJsonObject CoreAudioPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "CoreAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty()) obj["device"] = QString::fromStdString(device.value());
    if (format.has_value()) obj["format"] = QString::fromStdString(format.value());
    if (exclusive.has_value()) obj["exclusive"] = exclusive.value();
    if (outputDoP.has_value()) obj["output_dop"] = outputDoP.value();
    if (dopEncoderFilter.has_value()) obj["dop_encoder_filter"] = QString::fromStdString(sdmFilterToString(dopEncoderFilter.value()));
    return obj;
}

QJsonObject WavFileCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "WavFile";
    obj["filename"] = QString::fromStdString(filename);
    if (extraSamples.has_value()) obj["extra_samples"] = extraSamples.value();
    return obj;
}

QJsonObject RawFileCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "RawFile";
    obj["channels"] = channels;
    obj["filename"] = QString::fromStdString(filename);
    obj["format"] = QString::fromStdString(format);
    if (skipBytes.has_value()) obj["skip_bytes"] = skipBytes.value();
    if (readBytes.has_value()) obj["read_bytes"] = readBytes.value();
    if (extraSamples.has_value()) obj["extra_samples"] = extraSamples.value();
    return obj;
}

QJsonObject RawFilePlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "File";
    obj["channels"] = channels;
    obj["filename"] = QString::fromStdString(filename);
    obj["format"] = QString::fromStdString(format);
    if (wavHeader.has_value()) obj["wav_header"] = wavHeader.value();
    return obj;
}

QJsonObject GeneratorCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "SignalGenerator";
    obj["channels"] = channels;
    obj["signal"] = signal.toJson();
    return obj;
}

QJsonObject CaptureDeviceConfig::toJson() const {
    switch (backend) {
    case AudioBackendType::CoreAudio: return coreAudio.toJson();
    case AudioBackendType::WavFile: return wavFile.toJson();
    case AudioBackendType::RawFile: return rawFile.toJson();
    case AudioBackendType::SignalGenerator: return generator.toJson();
    }
    return coreAudio.toJson();
}

QJsonObject PlaybackDeviceConfig::toJson() const {
    switch (backend) {
    case AudioBackendType::CoreAudio: return coreAudio.toJson();
    case AudioBackendType::RawFile: return rawFile.toJson();
    case AudioBackendType::WavFile: return rawFile.toJson();
    case AudioBackendType::SignalGenerator: return coreAudio.toJson();
    }
    return coreAudio.toJson();
}

QJsonObject ResamplerConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(resamplerTypeToString(type));
    if (profile.has_value()) obj["profile"] = QString::fromStdString(profile.value());
    if (interpolation.has_value()) obj["interpolation"] = QString::fromStdString(interpolation.value());
    if (sincLen.has_value()) obj["sinc_len"] = sincLen.value();
    if (oversamplingFactor.has_value()) obj["oversampling_factor"] = oversamplingFactor.value();
    if (window.has_value()) obj["window"] = QString::fromStdString(window.value());
    if (fCutoff.has_value()) obj["f_cutoff"] = fCutoff.value();
    if (appleQuality.has_value()) {
        std::string qStr = "High";
        switch (appleQuality.value()) {
        case AppleResamplerQuality::Min: qStr = "Min"; break;
        case AppleResamplerQuality::Low: qStr = "Low"; break;
        case AppleResamplerQuality::Medium: qStr = "Medium"; break;
        case AppleResamplerQuality::High: qStr = "High"; break;
        case AppleResamplerQuality::Max: qStr = "Max"; break;
        }
        obj["apple_quality"] = QString::fromStdString(qStr);
    }
    if (appleComplexity.has_value()) {
        std::string cStr = "Normal";
        switch (appleComplexity.value()) {
        case AppleResamplerComplexity::Linear: cStr = "Linear"; break;
        case AppleResamplerComplexity::Normal: cStr = "Normal"; break;
        case AppleResamplerComplexity::Mastering: cStr = "Mastering"; break;
        case AppleResamplerComplexity::MinimumPhase: cStr = "MinimumPhase"; break;
        }
        obj["apple_complexity"] = QString::fromStdString(cStr);
    }
    return obj;
}

QJsonObject DevicesConfig::toJson() const {
    QJsonObject obj;
    obj["samplerate"] = samplerate;
    obj["chunksize"] = chunksize;
    obj["capture"] = capture.toJson();
    obj["playback"] = playback.toJson();

    if (enableRateAdjust.has_value()) obj["enable_rate_adjust"] = enableRateAdjust.value();
    if (targetLevel.has_value()) obj["target_level"] = targetLevel.value();
    if (adjustPeriod.has_value()) obj["adjust_period"] = adjustPeriod.value();
    if (resampler.has_value()) obj["resampler"] = resampler.value().toJson();
    if (captureSamplerate.has_value()) obj["capture_samplerate"] = captureSamplerate.value();
    if (silenceThreshold.has_value()) obj["silence_threshold"] = silenceThreshold.value();
    if (silenceTimeout.has_value()) obj["silence_timeout"] = silenceTimeout.value();
    if (volumeRampTime.has_value()) obj["volume_ramp_time"] = volumeRampTime.value();
    if (volumeLimit.has_value()) obj["volume_limit"] = volumeLimit.value();
    if (queuelimit.has_value()) obj["queuelimit"] = queuelimit.value();
    if (stopOnRateChange.has_value()) obj["stop_on_rate_change"] = stopOnRateChange.value();
    if (rateMeasureInterval.has_value()) obj["rate_measure_interval"] = rateMeasureInterval.value();
    if (multithreaded.has_value()) obj["multithreaded"] = multithreaded.value();
    if (workerThreads.has_value()) obj["worker_threads"] = workerThreads.value();

    return obj;
}

QJsonObject GainParameters::toJson() const {
    QJsonObject obj;
    if (gain.has_value()) obj["gain"] = gain.value();
    if (scale.has_value()) obj["scale"] = QString::fromStdString(gainScaleToString(scale.value()));
    if (inverted.has_value()) obj["inverted"] = inverted.value();
    if (mute.has_value()) obj["mute"] = mute.value();
    return obj;
}

QJsonObject LoudnessParameters::toJson() const {
    QJsonObject obj;
    if (referenceLevel.has_value()) obj["reference_level"] = referenceLevel.value();
    if (highBoost.has_value()) obj["high_boost"] = highBoost.value();
    if (lowBoost.has_value()) obj["low_boost"] = lowBoost.value();
    if (attenuateMid.has_value()) obj["attenuate_mid"] = attenuateMid.value();
    if (fader.has_value()) obj["fader"] = QString::fromStdString(faderToString(fader.value()));
    return obj;
}

QJsonObject ConvParameters::toJson() const {
    QJsonObject obj;
    std::string typeStr = "Raw";
    switch (type) {
    case ConvType::Values: typeStr = "Values"; break;
    case ConvType::Wav: typeStr = "Wav"; break;
    case ConvType::Raw: typeStr = "Raw"; break;
    case ConvType::Dummy: typeStr = "Dummy"; break;
    }
    obj["type"] = QString::fromStdString(typeStr);
    if (!values.empty()) {
        QJsonArray arr;
        for (double v : values) arr.append(v);
        obj["values"] = arr;
    }
    if (!filename.empty()) obj["filename"] = QString::fromStdString(filename);
    if (!format.empty()) obj["format"] = QString::fromStdString(format);
    if (channel.has_value()) obj["channel"] = channel.value();
    if (length.has_value()) obj["length"] = length.value();
    if (skipBytesLines.has_value()) obj["skip_bytes_lines"] = skipBytesLines.value();
    if (readBytesLines.has_value()) obj["read_bytes_lines"] = readBytesLines.value();
    return obj;
}

QJsonObject DelayParameters::toJson() const {
    QJsonObject obj;
    obj["delay"] = delay;
    if (unit.has_value()) obj["unit"] = QString::fromStdString(delayUnitToString(unit.value()));
    if (subsample.has_value()) obj["subsample"] = subsample.value();
    return obj;
}

QJsonObject BiquadComboParameters::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(biquadComboTypeToString(type));
    if (freq.has_value()) obj["freq"] = freq.value();
    if (order.has_value()) obj["order"] = order.value();
    if (gain.has_value()) obj["gain"] = gain.value();
    if (fls.has_value()) obj["fls"] = fls.value();
    if (qls.has_value()) obj["qls"] = qls.value();
    if (gls.has_value()) obj["gls"] = gls.value();
    if (fp1.has_value()) obj["fp1"] = fp1.value();
    if (qp1.has_value()) obj["qp1"] = qp1.value();
    if (gp1.has_value()) obj["gp1"] = gp1.value();
    if (fp2.has_value()) obj["fp2"] = fp2.value();
    if (qp2.has_value()) obj["qp2"] = qp2.value();
    if (gp2.has_value()) obj["gp2"] = gp2.value();
    if (fp3.has_value()) obj["fp3"] = fp3.value();
    if (qp3.has_value()) obj["qp3"] = qp3.value();
    if (gp3.has_value()) obj["gp3"] = gp3.value();
    if (fhs.has_value()) obj["fhs"] = fhs.value();
    if (qhs.has_value()) obj["qhs"] = qhs.value();
    if (ghs.has_value()) obj["ghs"] = ghs.value();
    if (freqMin.has_value()) obj["freq_min"] = freqMin.value();
    if (freqMax.has_value()) obj["freq_max"] = freqMax.value();
    if (!gains.empty()) {
        QJsonArray arr;
        for (double g : gains) arr.append(g);
        obj["gains"] = arr;
    }
    return obj;
}

QJsonObject DiffEqParameters::toJson() const {
    QJsonObject obj;
    if (!a.empty()) {
        QJsonArray arr;
        for (double val : a) arr.append(val);
        obj["a"] = arr;
    }
    if (!b.empty()) {
        QJsonArray arr;
        for (double val : b) arr.append(val);
        obj["b"] = arr;
    }
    return obj;
}

QJsonObject DitherParameters::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(ditherTypeToString(type));
    obj["bits"] = bits;
    if (amplitude.has_value()) obj["amplitude"] = amplitude.value();
    return obj;
}

QJsonObject LimiterParameters::toJson() const {
    QJsonObject obj;
    obj["clip_limit"] = clipLimit;
    if (softClip.has_value()) obj["soft_clip"] = softClip.value();
    return obj;
}

QJsonObject LookaheadLimiterParameters::toJson() const {
    QJsonObject obj;
    obj["limit"] = limit;
    obj["attack"] = attack;
    obj["release"] = release;
    if (unit.has_value()) obj["unit"] = QString::fromStdString(delayUnitToString(unit.value()));
    return obj;
}

QJsonObject VolumeParameters::toJson() const {
    QJsonObject obj;
    if (rampTime.has_value()) obj["ramp_time"] = rampTime.value();
    if (limit.has_value()) obj["limit"] = limit.value();
    if (fader.has_value()) obj["fader"] = QString::fromStdString(faderToString(fader.value()));
    return obj;
}

QJsonObject FilterConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(filterTypeToString(type));
    QJsonObject pObj;
    switch (type) {
    case FilterType::Gain: pObj = gainParams.toJson(); break;
    case FilterType::Volume: pObj = volumeParams.toJson(); break;
    case FilterType::Loudness: pObj = loudnessParams.toJson(); break;
    case FilterType::Biquad: {
        QJsonObject bObj;
        if (biquadParams.type.has_value()) bObj["type"] = QString::fromStdString(biquadTypeToString(biquadParams.type.value()));
        if (biquadParams.freq.has_value()) bObj["freq"] = biquadParams.freq.value();
        if (biquadParams.gain.has_value()) bObj["gain"] = biquadParams.gain.value();
        if (biquadParams.q.has_value()) bObj["q"] = biquadParams.q.value();
        if (biquadParams.bandwidth.has_value()) bObj["bandwidth"] = biquadParams.bandwidth.value();
        if (biquadParams.slope.has_value()) bObj["slope"] = biquadParams.slope.value();
        if (biquadParams.b0.has_value()) bObj["b0"] = biquadParams.b0.value();
        if (biquadParams.b1.has_value()) bObj["b1"] = biquadParams.b1.value();
        if (biquadParams.b2.has_value()) bObj["b2"] = biquadParams.b2.value();
        if (biquadParams.a1.has_value()) bObj["a1"] = biquadParams.a1.value();
        if (biquadParams.a2.has_value()) bObj["a2"] = biquadParams.a2.value();
        if (biquadParams.freqNotch.has_value()) bObj["freq_z"] = biquadParams.freqNotch.value();
        if (biquadParams.freqPole.has_value()) bObj["freq_p"] = biquadParams.freqPole.value();
        if (biquadParams.qP.has_value()) bObj["q_p"] = biquadParams.qP.value();
        if (biquadParams.normalizeAtDc.has_value()) bObj["normalize_at_dc"] = biquadParams.normalizeAtDc.value();
        if (biquadParams.freqAct.has_value()) bObj["freq_act"] = biquadParams.freqAct.value();
        if (biquadParams.qAct.has_value()) bObj["q_act"] = biquadParams.qAct.value();
        if (biquadParams.freqTarget.has_value()) bObj["freq_target"] = biquadParams.freqTarget.value();
        if (biquadParams.qTarget.has_value()) bObj["q_target"] = biquadParams.qTarget.value();
        pObj = bObj;
        break;
    }
    case FilterType::Conv: pObj = convParams.toJson(); break;
    case FilterType::Delay: pObj = delayParams.toJson(); break;
    case FilterType::BiquadCombo: pObj = comboParams.toJson(); break;
    case FilterType::DiffEq: pObj = diffEqParams.toJson(); break;
    case FilterType::Dither: pObj = ditherParams.toJson(); break;
    case FilterType::Limiter: pObj = limiterParams.toJson(); break;
    case FilterType::LookaheadLimiter: pObj = lookaheadParams.toJson(); break;
    }
    obj["parameters"] = pObj;
    return obj;
}

QJsonObject MixerSource::toJson() const {
    QJsonObject obj;
    obj["channel"] = channel;
    if (gain.has_value()) obj["gain"] = gain.value();
    if (inverted.has_value()) obj["inverted"] = inverted.value();
    if (mute.has_value()) obj["mute"] = mute.value();
    if (scale.has_value()) obj["scale"] = QString::fromStdString(gainScaleToString(scale.value()));
    return obj;
}

QJsonObject MixerMapping::toJson() const {
    QJsonObject obj;
    obj["dest"] = dest;
    QJsonArray srcArr;
    for (const auto& s : sources) srcArr.append(s.toJson());
    obj["sources"] = srcArr;
    if (mute.has_value()) obj["mute"] = mute.value();
    return obj;
}

QJsonObject MixerConfig::toJson() const {
    QJsonObject obj;
    QJsonObject chObj;
    chObj["in"] = channelsIn;
    chObj["out"] = channelsOut;
    obj["channels"] = chObj;

    QJsonArray mapArr;
    for (const auto& m : mapping) mapArr.append(m.toJson());
    obj["mapping"] = mapArr;

    if (description.has_value() && !description.value().empty()) {
        obj["description"] = QString::fromStdString(description.value());
    }
    return obj;
}

QJsonObject CompressorParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    if (!monitorChannels.empty()) {
        QJsonArray arr; for (int ch : monitorChannels) arr.append(ch);
        obj["monitor_channels"] = arr;
    }
    if (!processChannels.empty()) {
        QJsonArray arr; for (int ch : processChannels) arr.append(ch);
        obj["process_channels"] = arr;
    }
    obj["attack"] = attack;
    obj["release"] = release;
    obj["threshold"] = threshold;
    obj["factor"] = factor;
    if (makeupGain.has_value()) obj["makeup_gain"] = makeupGain.value();
    if (softClip.has_value()) obj["soft_clip"] = softClip.value();
    if (clipLimit.has_value()) obj["clip_limit"] = clipLimit.value();
    return obj;
}

QJsonObject NoiseGateParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    if (!monitorChannels.empty()) {
        QJsonArray arr; for (int ch : monitorChannels) arr.append(ch);
        obj["monitor_channels"] = arr;
    }
    if (!processChannels.empty()) {
        QJsonArray arr; for (int ch : processChannels) arr.append(ch);
        obj["process_channels"] = arr;
    }
    obj["attack"] = attack;
    obj["release"] = release;
    obj["threshold"] = threshold;
    obj["attenuation"] = attenuation;
    return obj;
}

QJsonObject RACEParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    obj["channel_a"] = channelA;
    obj["channel_b"] = channelB;
    obj["delay"] = delay;
    if (subsampleDelay.has_value()) obj["subsample_delay"] = subsampleDelay.value();
    if (delayUnit.has_value()) obj["delay_unit"] = QString::fromStdString(delayUnitToString(delayUnit.value()));
    obj["attenuation"] = attenuation;
    return obj;
}

QJsonObject ProcessorConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(processorTypeToString(type));
    QJsonObject pObj;
    switch (type) {
    case ProcessorType::Compressor: pObj = compressorParams.toJson(); break;
    case ProcessorType::NoiseGate: pObj = noiseGateParams.toJson(); break;
    case ProcessorType::RACE: pObj = raceParams.toJson(); break;
    }
    obj["parameters"] = pObj;
    return obj;
}

QJsonObject PipelineStep::toJson() const {
    QJsonObject obj;
    std::string typeStr = "Filter";
    switch (type) {
    case PipelineStepType::Filter: typeStr = "Filter"; break;
    case PipelineStepType::Mixer: typeStr = "Mixer"; break;
    case PipelineStepType::Processor: typeStr = "Processor"; break;
    }
    obj["type"] = QString::fromStdString(typeStr);
    if (channel.has_value()) obj["channel"] = channel.value();
    if (!channels.empty()) {
        QJsonArray arr; for (int c : channels) arr.append(c);
        obj["channels"] = arr;
    }
    if (type == PipelineStepType::Filter) {
        QJsonArray arr;
        if (!names.empty()) {
            for (const auto& n : names) arr.append(QString::fromStdString(n));
        } else if (name.has_value()) {
            arr.append(QString::fromStdString(name.value()));
        }
        obj["names"] = arr;
    } else {
        if (name.has_value()) obj["name"] = QString::fromStdString(name.value());
        if (!names.empty()) {
            QJsonArray arr; for (const auto& n : names) arr.append(QString::fromStdString(n));
            obj["names"] = arr;
        }
    }
    if (bypassed.has_value()) obj["bypassed"] = bypassed.value();
    return obj;
}

QJsonObject DSPConfiguration::toJsonObject() const {
    QJsonObject root;
    root["devices"] = devices.toJson();

    if (!filters.empty()) {
        QJsonObject fObj;
        for (const auto& [k, v] : filters) {
            fObj[QString::fromStdString(k)] = v.toJson();
        }
        root["filters"] = fObj;
    }

    if (!mixers.empty()) {
        QJsonObject mObj;
        for (const auto& [k, v] : mixers) {
            mObj[QString::fromStdString(k)] = v.toJson();
        }
        root["mixers"] = mObj;
    }

    if (!processors.empty()) {
        QJsonObject pObj;
        for (const auto& [k, v] : processors) {
            pObj[QString::fromStdString(k)] = v.toJson();
        }
        root["processors"] = pObj;
    }

    if (!pipeline.empty()) {
        QJsonArray pipeArr;
        for (const auto& step : pipeline) {
            pipeArr.append(step.toJson());
        }
        root["pipeline"] = pipeArr;
    }

    return root;
}

std::string DSPConfiguration::toJsonString() const {
    QJsonDocument doc(toJsonObject());
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

void DSPConfiguration::validate() const {
    if (devices.samplerate <= 0) {
        throw std::runtime_error("Sample rate must be positive");
    }
    if (devices.chunksize < 1 || devices.chunksize > 1000000) {
        throw std::runtime_error("Chunk size must be between 1 and 1000000");
    }
}
