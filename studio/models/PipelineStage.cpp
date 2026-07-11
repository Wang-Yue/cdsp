#include "models/PipelineStage.h"
#include <cmath>

std::string stageCategoryToString(StageCategory cat) {
    switch (cat) {
    case StageCategory::Volume: return "Volume & Balance";
    case StageCategory::EQ: return "EQ & Convolution";
    case StageCategory::Dynamics: return "Dynamics & Dither";
    case StageCategory::Delay: return "Delay";
    case StageCategory::Matrix: return "Matrix & Processing";
    }
    return "Volume & Balance";
}

std::string stageTypeToString(StageType type) {
    switch (type) {
    case StageType::Balance: return "Balance";
    case StageType::Width: return "Stereo Width";
    case StageType::MSProc: return "M/S Processing";
    case StageType::PhaseInvert: return "Phase Invert";
    case StageType::Crossfeed: return "Headphone Crossfeed";
    case StageType::SplitWidth: return "Frequency Split Width";
    case StageType::EQ: return "Parametric EQ";
    case StageType::GraphicEQ: return "Graphic EQ";
    case StageType::Convolution: return "FIR Convolution";
    case StageType::Loudness: return "Fletcher-Munson Loudness";
    case StageType::Emphasis: return "Pre/De-Emphasis";
    case StageType::DCProtection: return "DC Protection";
    case StageType::Gain: return "Gain";
    case StageType::Delay: return "Delay";
    case StageType::LookaheadLimiter: return "Lookahead Limiter";
    case StageType::Limiter: return "Peak Limiter";
    case StageType::Volume: return "Volume Fader";
    case StageType::MatrixMixer: return "Matrix Mixer";
    case StageType::Compressor: return "Dynamic Compressor";
    case StageType::NoiseGate: return "Noise Gate";
    case StageType::RACE: return "RACE Crosstalk Cancel";
    case StageType::Dither: return "Bit Dither";
    case StageType::DiffEq: return "Custom DiffEq";
    case StageType::BiquadCombo: return "Biquad Combo";
    }
    return "Gain";
}

StageCategory stageTypeToCategory(StageType type) {
    switch (type) {
    case StageType::Balance:
    case StageType::Width:
    case StageType::MSProc:
    case StageType::PhaseInvert:
    case StageType::Volume:
    case StageType::Gain:
    case StageType::Loudness:
        return StageCategory::Volume;
    case StageType::EQ:
    case StageType::GraphicEQ:
    case StageType::Convolution:
    case StageType::BiquadCombo:
    case StageType::DiffEq:
        return StageCategory::EQ;
    case StageType::Compressor:
    case StageType::NoiseGate:
    case StageType::Limiter:
    case StageType::LookaheadLimiter:
    case StageType::RACE:
    case StageType::Dither:
        return StageCategory::Dynamics;
    case StageType::Delay:
        return StageCategory::Delay;
    case StageType::MatrixMixer:
    case StageType::Crossfeed:
    case StageType::SplitWidth:
    case StageType::DCProtection:
    case StageType::Emphasis:
        return StageCategory::Matrix;
    }
    return StageCategory::Volume;
}

std::string stageTypeToIcon(StageType type) {
    switch (type) {
    case StageType::Balance: return "slider.horizontal.2.square";
    case StageType::Width: return "arrow.left.and.right.square";
    case StageType::MSProc: return "square.split.2x1";
    case StageType::PhaseInvert: return "arrow.triangle.2.circlepath.circle";
    case StageType::Crossfeed: return "headphones";
    case StageType::SplitWidth: return "slider.horizontal.below.square.and.square";
    case StageType::EQ: return "slider.horizontal.3";
    case StageType::GraphicEQ: return "chart.bar.fill";
    case StageType::Convolution: return "waveform.badge.magnifyingglass";
    case StageType::Loudness: return "speaker.wave.3";
    case StageType::Emphasis: return "waveform.path.badge.plus";
    case StageType::DCProtection: return "shield";
    case StageType::Gain: return "plusminus.circle";
    case StageType::Delay: return "clock";
    case StageType::LookaheadLimiter: return "bolt.shield";
    case StageType::Limiter: return "square.split.1x2";
    case StageType::Volume: return "speaker.wave.2";
    case StageType::MatrixMixer: return "grid";
    case StageType::Compressor: return "waveform.path.ecg";
    case StageType::NoiseGate: return "door.french.closed";
    case StageType::RACE: return "antenna.radiowaves.left.and.right";
    case StageType::Dither: return "square.grid.3x3.topleft.filled";
    case StageType::DiffEq: return "function";
    case StageType::BiquadCombo: return "square.stack.3d.up";
    }
    return "slider.horizontal.3";
}

PipelineStage::PipelineStage() : id(QUuid::createUuid()), type(StageType::Gain), name("Gain") {}

PipelineStage::PipelineStage(StageType type, const std::string& name)
    : id(QUuid::createUuid()), type(type), name(name.empty() ? stageTypeToString(type) : name) {}

QJsonObject PipelineStage::toJson() const {
    QJsonObject obj;
    obj["id"] = id.toString();
    obj["name"] = QString::fromStdString(name);
    obj["type"] = QString::fromStdString(stageTypeToString(type));
    obj["isEnabled"] = isEnabled;
    if (!channels.empty()) {
        QJsonArray arr; for (int c : channels) arr.append(c);
        obj["channels"] = arr;
    }
    if (!monitorChannels.empty()) {
        QJsonArray arr; for (int c : monitorChannels) arr.append(c);
        obj["monitorChannels"] = arr;
    }
    obj["leftChannel"] = leftChannel;
    obj["rightChannel"] = rightChannel;
    obj["balanceOffset"] = balanceOffset;
    obj["widthFactor"] = widthFactor;
    obj["invertLeft"] = invertLeft;
    obj["invertRight"] = invertRight;
    obj["crossfeedCutoff"] = crossfeedCutoff;
    obj["crossfeedFeedDB"] = crossfeedFeedDB;
    obj["splitFreq"] = splitFreq;
    obj["lowWidth"] = lowWidth;
    obj["highWidth"] = highWidth;
    if (eqPresetId.has_value()) obj["eqPresetId"] = eqPresetId.value().toString();
    if (convPresetId.has_value()) obj["convPresetId"] = convPresetId.value().toString();
    obj["loudnessRefLevel"] = loudnessRefLevel;
    obj["loudnessHighBoost"] = loudnessHighBoost;
    obj["loudnessLowBoost"] = loudnessLowBoost;
    obj["loudnessAttenuateMid"] = loudnessAttenuateMid;
    obj["deEmphasis"] = deEmphasis;
    obj["dcCutoffFreq"] = dcCutoffFreq;
    obj["gainDB"] = gainDB;
    obj["gainInverted"] = gainInverted;
    obj["gainMuted"] = gainMuted;
    obj["delayValue"] = delayValue;
    obj["delayUnit"] = QString::fromStdString(delayUnitToString(delayUnit));
    obj["limiterThreshold"] = limiterThreshold;
    obj["limiterSoftClip"] = limiterSoftClip;
    obj["lookaheadLimit"] = lookaheadLimit;
    obj["lookaheadAttack"] = lookaheadAttack;
    obj["lookaheadRelease"] = lookaheadRelease;
    obj["ditherType"] = QString::fromStdString(ditherTypeToString(ditherType));
    obj["ditherBits"] = ditherBits;
    obj["mixerConfig"] = mixerConfig.toJson();
    obj["compressorParams"] = compressorParams.toJson();
    obj["noiseGateParams"] = noiseGateParams.toJson();
    obj["raceParams"] = raceParams.toJson();
    obj["comboParams"] = comboParams.toJson();
    if (!diffEqA.empty()) {
        QJsonArray arr; for (double val : diffEqA) arr.append(val);
        obj["diffEqA"] = arr;
    }
    if (!diffEqB.empty()) {
        QJsonArray arr; for (double val : diffEqB) arr.append(val);
        obj["diffEqB"] = arr;
    }
    if (!graphicEqGains.empty()) {
        QJsonArray arr; for (double val : graphicEqGains) arr.append(val);
        obj["graphicEqGains"] = arr;
    }
    return obj;
}

PipelineStage PipelineStage::fromJson(const QJsonObject& json) {
    PipelineStage s;
    if (json.contains("id")) s.id = QUuid::fromString(json["id"].toString());
    if (json.contains("name")) s.name = json["name"].toString().toStdString();
    if (json.contains("type")) {
        std::string typeStr = json["type"].toString().toStdString();
        for (StageType st : {
            StageType::Balance, StageType::Width, StageType::MSProc, StageType::PhaseInvert, StageType::Crossfeed, StageType::SplitWidth,
            StageType::EQ, StageType::GraphicEQ, StageType::Convolution, StageType::Loudness, StageType::Emphasis, StageType::DCProtection,
            StageType::Gain, StageType::Delay, StageType::LookaheadLimiter, StageType::Limiter, StageType::Volume, StageType::MatrixMixer,
            StageType::Compressor, StageType::NoiseGate, StageType::RACE, StageType::Dither, StageType::DiffEq, StageType::BiquadCombo
        }) {
            if (stageTypeToString(st) == typeStr) { s.type = st; break; }
        }
    }
    if (json.contains("isEnabled")) s.isEnabled = json["isEnabled"].toBool();
    if (json.contains("channels")) {
        QJsonArray arr = json["channels"].toArray();
        for (const auto& c : arr) s.channels.push_back(c.toInt());
    }
    if (json.contains("monitorChannels")) {
        QJsonArray arr = json["monitorChannels"].toArray();
        for (const auto& c : arr) s.monitorChannels.push_back(c.toInt());
    }
    if (json.contains("leftChannel")) s.leftChannel = json["leftChannel"].toInt();
    if (json.contains("rightChannel")) s.rightChannel = json["rightChannel"].toInt();
    if (json.contains("balanceOffset")) s.balanceOffset = json["balanceOffset"].toDouble();
    if (json.contains("widthFactor")) s.widthFactor = json["widthFactor"].toDouble();
    if (json.contains("invertLeft")) s.invertLeft = json["invertLeft"].toBool();
    if (json.contains("invertRight")) s.invertRight = json["invertRight"].toBool();
    if (json.contains("crossfeedCutoff")) s.crossfeedCutoff = json["crossfeedCutoff"].toDouble();
    if (json.contains("crossfeedFeedDB")) s.crossfeedFeedDB = json["crossfeedFeedDB"].toDouble();
    if (json.contains("splitFreq")) s.splitFreq = json["splitFreq"].toDouble();
    if (json.contains("lowWidth")) s.lowWidth = json["lowWidth"].toDouble();
    if (json.contains("highWidth")) s.highWidth = json["highWidth"].toDouble();
    if (json.contains("eqPresetId")) s.eqPresetId = QUuid::fromString(json["eqPresetId"].toString());
    if (json.contains("convPresetId")) s.convPresetId = QUuid::fromString(json["convPresetId"].toString());
    if (json.contains("loudnessRefLevel")) s.loudnessRefLevel = json["loudnessRefLevel"].toDouble();
    if (json.contains("loudnessHighBoost")) s.loudnessHighBoost = json["loudnessHighBoost"].toDouble();
    if (json.contains("loudnessLowBoost")) s.loudnessLowBoost = json["loudnessLowBoost"].toDouble();
    if (json.contains("loudnessAttenuateMid")) s.loudnessAttenuateMid = json["loudnessAttenuateMid"].toBool();
    if (json.contains("deEmphasis")) s.deEmphasis = json["deEmphasis"].toBool();
    if (json.contains("dcCutoffFreq")) s.dcCutoffFreq = json["dcCutoffFreq"].toDouble();
    if (json.contains("gainDB")) s.gainDB = json["gainDB"].toDouble();
    if (json.contains("gainInverted")) s.gainInverted = json["gainInverted"].toBool();
    if (json.contains("gainMuted")) s.gainMuted = json["gainMuted"].toBool();
    if (json.contains("delayValue")) s.delayValue = json["delayValue"].toDouble();
    if (json.contains("delayUnit")) s.delayUnit = stringToDelayUnit(json["delayUnit"].toString().toStdString());
    if (json.contains("limiterThreshold")) s.limiterThreshold = json["limiterThreshold"].toDouble();
    if (json.contains("limiterSoftClip")) s.limiterSoftClip = json["limiterSoftClip"].toBool();
    if (json.contains("lookaheadLimit")) s.lookaheadLimit = json["lookaheadLimit"].toDouble();
    if (json.contains("lookaheadAttack")) s.lookaheadAttack = json["lookaheadAttack"].toDouble();
    if (json.contains("lookaheadRelease")) s.lookaheadRelease = json["lookaheadRelease"].toDouble();
    if (json.contains("ditherBits")) s.ditherBits = json["ditherBits"].toInt();
    if (json.contains("mixerConfig")) s.mixerConfig = MixerConfig::fromJson(json["mixerConfig"].toObject());
    if (json.contains("compressorParams")) s.compressorParams = CompressorParameters::fromJson(json["compressorParams"].toObject());
    if (json.contains("noiseGateParams")) s.noiseGateParams = NoiseGateParameters::fromJson(json["noiseGateParams"].toObject());
    if (json.contains("raceParams")) s.raceParams = RACEParameters::fromJson(json["raceParams"].toObject());
    if (json.contains("comboParams")) s.comboParams = BiquadComboParameters::fromJson(json["comboParams"].toObject());
    if (json.contains("diffEqA")) {
        QJsonArray arr = json["diffEqA"].toArray();
        for (const auto& v : arr) s.diffEqA.push_back(v.toDouble());
    }
    if (json.contains("diffEqB")) {
        QJsonArray arr = json["diffEqB"].toArray();
        for (const auto& v : arr) s.diffEqB.push_back(v.toDouble());
    }
    if (json.contains("graphicEqGains")) {
        QJsonArray arr = json["graphicEqGains"].toArray();
        for (const auto& v : arr) s.graphicEqGains.push_back(v.toDouble());
    }
    return s;
}

bool PipelineStage::operator==(const PipelineStage& other) const {
    return id == other.id && name == other.name && type == other.type && isEnabled == other.isEnabled;
}

StageBuildResult StageBuilders::buildStage(
    const PipelineStage& stage,
    int sampleRate,
    int channelCount,
    const std::map<QUuid, EQPreset>& eqPresets,
    const std::map<QUuid, ConvolutionPreset>& convPresets
) {
    StageBuildResult res;
    if (!stage.isEnabled) return res;

    std::string baseKey = "stage_" + stage.id.toString(QUuid::WithoutBraces).toStdString();

    auto getTargetChannels = [&stage, channelCount]() -> std::vector<int> {
        if (!stage.channels.empty()) return stage.channels;
        std::vector<int> all;
        for (int ch = 0; ch < channelCount; ++ch) all.push_back(ch);
        return all;
    };

    switch (stage.type) {
    case StageType::Balance: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        double leftLin = 1.0 - std::max(0.0, stage.balanceOffset);
        double rightLin = 1.0 + std::min(0.0, stage.balanceOffset);
        double leftDB = leftLin > 0.0 ? 20.0 * std::log10(leftLin) : -100.0;
        double rightDB = rightLin > 0.0 ? 20.0 * std::log10(rightLin) : -100.0;

        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int ch = 0; ch < channelCount; ++ch) {
            MixerMapping m;
            m.dest = ch;
            if (ch == lCh) m.sources.push_back(MixerSource{lCh, leftDB, false});
            else if (ch == rCh) m.sources.push_back(MixerSource{rCh, rightDB, false});
            else m.sources.push_back(MixerSource{ch, 0.0, false});
            mc.mapping.push_back(m);
        }
        std::string mKey = baseKey + "_balance";
        res.mixers[mKey] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = mKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::Width: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        double w = stage.widthFactor;
        double ll = (1.0 + w) / 2.0;
        double lr = (1.0 - w) / 2.0;

        auto makeSources = [](int leftCh, int rightCh, double ch0, double ch1) {
            std::vector<MixerSource> srcs;
            if (std::abs(ch0) > 1e-6) srcs.push_back(MixerSource{leftCh, 20.0 * std::log10(std::abs(ch0)), ch0 < 0});
            if (std::abs(ch1) > 1e-6) srcs.push_back(MixerSource{rightCh, 20.0 * std::log10(std::abs(ch1)), ch1 < 0});
            return srcs;
        };

        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int ch = 0; ch < channelCount; ++ch) {
            MixerMapping m;
            m.dest = ch;
            if (ch == lCh) m.sources = makeSources(lCh, rCh, ll, lr);
            else if (ch == rCh) m.sources = makeSources(lCh, rCh, lr, ll);
            else m.sources.push_back(MixerSource{ch, 0.0, false});
            mc.mapping.push_back(m);
        }
        std::string mKey = baseKey + "_width";
        res.mixers[mKey] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = mKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::MSProc: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int ch = 0; ch < channelCount; ++ch) {
            MixerMapping m;
            m.dest = ch;
            if (ch == lCh) {
                m.sources.push_back(MixerSource{lCh, -6.02, false});
                m.sources.push_back(MixerSource{rCh, -6.02, false});
            } else if (ch == rCh) {
                m.sources.push_back(MixerSource{lCh, -6.02, false});
                m.sources.push_back(MixerSource{rCh, -6.02, true});
            } else {
                m.sources.push_back(MixerSource{ch, 0.0, false});
            }
            mc.mapping.push_back(m);
        }
        std::string mKey = baseKey + "_msproc";
        res.mixers[mKey] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = mKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::PhaseInvert: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        FilterConfig f;
        f.type = FilterType::Gain;
        f.gainParams.gain = 0.0;
        f.gainParams.inverted = true;
        std::string fKey = baseKey + "_phase_invert";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        if (stage.invertLeft) step.channels.push_back(lCh);
        if (stage.invertRight) step.channels.push_back(rCh);
        if (!step.channels.empty()) res.steps.push_back(step);
        break;
    }

    case StageType::Crossfeed: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        // Step 1: 2->4 Split Mixer
        MixerConfig m1;
        m1.channelsIn = channelCount;
        m1.channelsOut = channelCount + 2;
        for (int ch = 0; ch < channelCount; ++ch) {
            if (ch == lCh) {
                MixerMapping map0, map1;
                map0.dest = 0; map0.sources.push_back(MixerSource{lCh, 0.0, false});
                map1.dest = 1; map1.sources.push_back(MixerSource{lCh, 0.0, false});
                m1.mapping.push_back(map0); m1.mapping.push_back(map1);
            } else if (ch == rCh) {
                MixerMapping map2, map3;
                map2.dest = 2; map2.sources.push_back(MixerSource{rCh, 0.0, false});
                map3.dest = 3; map3.sources.push_back(MixerSource{rCh, 0.0, false});
                m1.mapping.push_back(map2); m1.mapping.push_back(map3);
            } else {
                MixerMapping m; m.dest = ch + 2; m.sources.push_back(MixerSource{ch, 0.0, false});
                m1.mapping.push_back(m);
            }
        }
        std::string m1Key = baseKey + "_xf_split";
        res.mixers[m1Key] = m1;

        FilterConfig fHi, fLo, fGain;
        fHi.type = FilterType::Biquad;
        fHi.biquadParams.type = BiquadType::Lowshelf;
        fHi.biquadParams.freq = stage.crossfeedCutoff;
        fHi.biquadParams.gain = stage.crossfeedFeedDB;
        fHi.biquadParams.q = 0.7071;

        fLo.type = FilterType::Biquad;
        fLo.biquadParams.type = BiquadType::LowpassFO;
        fLo.biquadParams.freq = stage.crossfeedCutoff;

        fGain.type = FilterType::Gain;
        fGain.gainParams.gain = stage.crossfeedFeedDB;

        std::string fHiKey = baseKey + "_hi";
        std::string fLoKey = baseKey + "_lo";
        std::string fGainKey = baseKey + "_gain";
        res.filters[fHiKey] = fHi;
        res.filters[fLoKey] = fLo;
        res.filters[fGainKey] = fGain;

        MixerConfig m2;
        m2.channelsIn = channelCount + 2;
        m2.channelsOut = channelCount;
        for (int ch = 0; ch < channelCount; ++ch) {
            MixerMapping map;
            map.dest = ch;
            if (ch == lCh) {
                map.sources.push_back(MixerSource{0, 0.0, false});
                map.sources.push_back(MixerSource{2, 0.0, false});
            } else if (ch == rCh) {
                map.sources.push_back(MixerSource{3, 0.0, false});
                map.sources.push_back(MixerSource{1, 0.0, false});
            } else {
                map.sources.push_back(MixerSource{ch + 2, 0.0, false});
            }
            m2.mapping.push_back(map);
        }
        std::string m2Key = baseKey + "_xf_join";
        res.mixers[m2Key] = m2;

        PipelineStep step1, step2Hi, step2Lo, step2Gain, step3;
        step1.type = PipelineStepType::Mixer; step1.name = m1Key;
        step2Hi.type = PipelineStepType::Filter; step2Hi.names.push_back(fHiKey); step2Hi.channels = {0, 3};
        step2Lo.type = PipelineStepType::Filter; step2Lo.names.push_back(fLoKey); step2Lo.channels = {1, 2};
        step2Gain.type = PipelineStepType::Filter; step2Gain.names.push_back(fGainKey); step2Gain.channels = {1, 2};
        step3.type = PipelineStepType::Mixer; step3.name = m2Key;

        res.steps.push_back(step1);
        res.steps.push_back(step2Hi);
        res.steps.push_back(step2Lo);
        res.steps.push_back(step2Gain);
        res.steps.push_back(step3);
        break;
    }

    case StageType::SplitWidth: {
        int lCh = stage.leftChannel;
        int rCh = stage.rightChannel;
        MixerConfig m1;
        m1.channelsIn = channelCount;
        m1.channelsOut = channelCount + 2;
        for (int ch = 0; ch < channelCount; ++ch) {
            if (ch == lCh) {
                MixerMapping map0, map2;
                map0.dest = 0; map0.sources.push_back(MixerSource{lCh, 0.0, false});
                map2.dest = 2; map2.sources.push_back(MixerSource{lCh, 0.0, false});
                m1.mapping.push_back(map0); m1.mapping.push_back(map2);
            } else if (ch == rCh) {
                MixerMapping map1, map3;
                map1.dest = 1; map1.sources.push_back(MixerSource{rCh, 0.0, false});
                map3.dest = 3; map3.sources.push_back(MixerSource{rCh, 0.0, false});
                m1.mapping.push_back(map1); m1.mapping.push_back(map3);
            } else {
                MixerMapping m; m.dest = ch + 2; m.sources.push_back(MixerSource{ch, 0.0, false});
                m1.mapping.push_back(m);
            }
        }
        std::string m1Key = baseKey + "_sw_split";
        res.mixers[m1Key] = m1;

        FilterConfig fLp, fHp;
        fLp.type = FilterType::BiquadCombo;
        fLp.comboParams.type = BiquadComboType::LinkwitzRileyLowpass;
        fLp.comboParams.freq = stage.splitFreq;
        fLp.comboParams.order = 4;

        fHp.type = FilterType::BiquadCombo;
        fHp.comboParams.type = BiquadComboType::LinkwitzRileyHighpass;
        fHp.comboParams.freq = stage.splitFreq;
        fHp.comboParams.order = 4;

        std::string fLpKey = baseKey + "_lp";
        std::string fHpKey = baseKey + "_hp";
        res.filters[fLpKey] = fLp;
        res.filters[fHpKey] = fHp;

        double wLow = stage.lowWidth;
        double wHigh = stage.highWidth;
        double c1L = 0.5 * (1.0 + wLow);
        double c2L = 0.5 * (1.0 - wLow);
        double c1H = 0.5 * (1.0 + wHigh);
        double c2H = 0.5 * (1.0 - wHigh);

        auto makeSrc = [](int srcCh, double gainVal) {
            MixerSource s;
            s.channel = srcCh;
            if (std::abs(gainVal) > 1e-6) {
                s.gain = 20.0 * std::log10(std::abs(gainVal));
                s.inverted = gainVal < 0;
            } else {
                s.mute = true;
            }
            return s;
        };

        MixerConfig m2;
        m2.channelsIn = channelCount + 2;
        m2.channelsOut = channelCount;
        for (int ch = 0; ch < channelCount; ++ch) {
            MixerMapping map;
            map.dest = ch;
            if (ch == lCh) {
                map.sources.push_back(makeSrc(0, c1L));
                map.sources.push_back(makeSrc(1, c2L));
                map.sources.push_back(makeSrc(2, c1H));
                map.sources.push_back(makeSrc(3, c2H));
            } else if (ch == rCh) {
                map.sources.push_back(makeSrc(0, c2L));
                map.sources.push_back(makeSrc(1, c1L));
                map.sources.push_back(makeSrc(2, c2H));
                map.sources.push_back(makeSrc(3, c1H));
            } else {
                map.sources.push_back(MixerSource{ch + 2, 0.0, false});
            }
            m2.mapping.push_back(map);
        }
        std::string m2Key = baseKey + "_sw_join";
        res.mixers[m2Key] = m2;

        PipelineStep step1, stepLp, stepHp, step2;
        step1.type = PipelineStepType::Mixer; step1.name = m1Key;
        stepLp.type = PipelineStepType::Filter; stepLp.names.push_back(fLpKey); stepLp.channels = {0, 1};
        stepHp.type = PipelineStepType::Filter; stepHp.names.push_back(fHpKey); stepHp.channels = {2, 3};
        step2.type = PipelineStepType::Mixer; step2.name = m2Key;

        res.steps.push_back(step1);
        res.steps.push_back(stepLp);
        res.steps.push_back(stepHp);
        res.steps.push_back(step2);
        break;
    }

    case StageType::GraphicEQ: {
        FilterConfig f;
        f.type = FilterType::BiquadCombo;
        f.comboParams.type = BiquadComboType::GraphicEqualizer;
        f.comboParams.freqMin = 20.0;
        f.comboParams.freqMax = 20000.0;
        f.comboParams.gains = stage.graphicEqGains;
        std::string fKey = baseKey + "_geq";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Emphasis: {
        FilterConfig f;
        f.type = FilterType::Biquad;
        f.biquadParams.type = BiquadType::Highshelf;
        f.biquadParams.freq = 5200.0;
        f.biquadParams.gain = stage.deEmphasis ? -9.5 : 9.5;
        f.biquadParams.q = 0.5;
        std::string fKey = baseKey + (stage.deEmphasis ? "_deemphasis" : "_preemphasis");
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Dither: {
        FilterConfig f;
        f.type = FilterType::Dither;
        f.ditherParams.type = stage.ditherType;
        f.ditherParams.bits = stage.ditherBits;
        std::string fKey = baseKey + "_dither";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::DiffEq: {
        FilterConfig f;
        f.type = FilterType::DiffEq;
        f.diffEqParams.a = stage.diffEqA;
        f.diffEqParams.b = stage.diffEqB;
        std::string fKey = baseKey + "_diffeq";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::BiquadCombo: {
        FilterConfig f;
        f.type = FilterType::BiquadCombo;
        f.comboParams = stage.comboParams;
        std::string fKey = baseKey + "_combo";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Gain: {
        FilterConfig f;
        f.type = FilterType::Gain;
        f.gainParams.gain = stage.gainDB;
        f.gainParams.inverted = stage.gainInverted;
        f.gainParams.mute = stage.gainMuted;
        std::string filterKey = baseKey + "_gain";
        res.filters[filterKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(filterKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Volume: {
        FilterConfig f;
        f.type = FilterType::Volume;
        f.volumeParams.fader = Fader::Main;
        std::string filterKey = baseKey + "_volume";
        res.filters[filterKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(filterKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Delay: {
        FilterConfig f;
        f.type = FilterType::Delay;
        f.delayParams.delay = stage.delayValue;
        f.delayParams.unit = stage.delayUnit;
        std::string filterKey = baseKey + "_delay";
        res.filters[filterKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(filterKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::EQ: {
        if (!stage.eqPresetId.has_value() || !eqPresets.count(stage.eqPresetId.value())) break;
        const auto& preset = eqPresets.at(stage.eqPresetId.value());

        if (preset.preampGain != 0.0) {
            FilterConfig pGain;
            pGain.type = FilterType::Gain;
            pGain.gainParams.gain = preset.preampGain;
            std::string pKey = baseKey + "_eq_preamp";
            res.filters[pKey] = pGain;

            PipelineStep step;
            step.type = PipelineStepType::Filter;
            step.names.push_back(pKey);
            step.channels = getTargetChannels();
            res.steps.push_back(step);
        }

        for (size_t i = 0; i < preset.bands.size(); ++i) {
            const auto& b = preset.bands[i];
            if (!b.isEnabled) continue;

            FilterConfig f;
            f.type = FilterType::Biquad;
            f.biquadParams.type = stringToBiquadType(eqBandTypeToString(b.type));

            switch (b.type) {
            case EQBandType::Free:
                f.biquadParams.b0 = b.b0; f.biquadParams.b1 = b.b1; f.biquadParams.b2 = b.b2;
                f.biquadParams.a1 = b.a1; f.biquadParams.a2 = b.a2;
                break;
            case EQBandType::GeneralNotch:
                f.biquadParams.freqNotch = b.freqNotch;
                f.biquadParams.freqPole = b.freqPole;
                f.biquadParams.qP = b.qPole;
                f.biquadParams.normalizeAtDc = b.normalizeAtDc;
                break;
            case EQBandType::LinkwitzTransform:
                f.biquadParams.freqAct = b.freqAct;
                f.biquadParams.qAct = b.qAct;
                f.biquadParams.freqTarget = b.freqTarget;
                f.biquadParams.qTarget = b.qTarget;
                break;
            default:
                f.biquadParams.freq = b.freq;
                if (eqBandTypeHasGain(b.type)) f.biquadParams.gain = b.gain;
                if (eqBandTypeHasQ(b.type)) f.biquadParams.q = b.q;
                break;
            }

            std::string fKey = baseKey + "_eq_b" + std::to_string(i + 1);
            res.filters[fKey] = f;

            PipelineStep step;
            step.type = PipelineStepType::Filter;
            step.names.push_back(fKey);
            step.channels = getTargetChannels();
            res.steps.push_back(step);
        }
        break;
    }

    case StageType::Convolution: {
        if (!stage.convPresetId.has_value() || !convPresets.count(stage.convPresetId.value())) break;
        const auto& preset = convPresets.at(stage.convPresetId.value());
        std::string irPath = preset.irPath(sampleRate);
        if (irPath.empty()) break;

        FilterConfig f;
        f.type = FilterType::Conv;
        f.convParams.type = ConvType::Raw;
        f.convParams.filename = irPath;
        f.convParams.format = "FLOAT64";
        std::string fKey = baseKey + "_conv";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::DCProtection: {
        FilterConfig f;
        f.type = FilterType::Biquad;
        f.biquadParams.type = BiquadType::HighpassFO;
        f.biquadParams.freq = stage.dcCutoffFreq;
        std::string fKey = baseKey + "_dcp";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Limiter: {
        FilterConfig f;
        f.type = FilterType::Limiter;
        f.limiterParams.clipLimit = stage.limiterThreshold;
        f.limiterParams.softClip = stage.limiterSoftClip;
        std::string fKey = baseKey + "_limiter";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::LookaheadLimiter: {
        FilterConfig f;
        f.type = FilterType::LookaheadLimiter;
        f.lookaheadParams.limit = stage.lookaheadLimit;
        f.lookaheadParams.attack = stage.lookaheadAttack;
        f.lookaheadParams.release = stage.lookaheadRelease;
        std::string fKey = baseKey + "_lookahead";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::Loudness: {
        FilterConfig f;
        f.type = FilterType::Loudness;
        f.loudnessParams.referenceLevel = stage.loudnessRefLevel;
        f.loudnessParams.highBoost = stage.loudnessHighBoost;
        f.loudnessParams.lowBoost = stage.loudnessLowBoost;
        f.loudnessParams.attenuateMid = stage.loudnessAttenuateMid;
        f.loudnessParams.fader = Fader::Main;
        std::string fKey = baseKey + "_loudness";
        res.filters[fKey] = f;

        PipelineStep step;
        step.type = PipelineStepType::Filter;
        step.names.push_back(fKey);
        step.channels = getTargetChannels();
        res.steps.push_back(step);
        break;
    }

    case StageType::MatrixMixer: {
        std::string mKey = baseKey + "_matrix";
        res.mixers[mKey] = stage.mixerConfig;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = mKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::Compressor: {
        ProcessorConfig p;
        p.type = ProcessorType::Compressor;
        p.compressorParams = stage.compressorParams;
        p.compressorParams.channels = channelCount;
        std::string pKey = baseKey + "_compressor";
        res.processors[pKey] = p;

        PipelineStep step;
        step.type = PipelineStepType::Processor;
        step.name = pKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::NoiseGate: {
        ProcessorConfig p;
        p.type = ProcessorType::NoiseGate;
        p.noiseGateParams = stage.noiseGateParams;
        p.noiseGateParams.channels = channelCount;
        std::string pKey = baseKey + "_gate";
        res.processors[pKey] = p;

        PipelineStep step;
        step.type = PipelineStepType::Processor;
        step.name = pKey;
        res.steps.push_back(step);
        break;
    }

    case StageType::RACE: {
        ProcessorConfig p;
        p.type = ProcessorType::RACE;
        p.raceParams = stage.raceParams;
        p.raceParams.channels = channelCount;
        std::string pKey = baseKey + "_race";
        res.processors[pKey] = p;

        PipelineStep step;
        step.type = PipelineStepType::Processor;
        step.name = pKey;
        res.steps.push_back(step);
        break;
    }

    default:
        break;
    }

    return res;
}
