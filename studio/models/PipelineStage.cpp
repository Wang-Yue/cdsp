#include "models/PipelineStage.h"

#include <algorithm>
#include <cmath>

std::string stageCategoryToString(StageCategory cat) {
    switch (cat) {
    case StageCategory::Filters:
        return "Filters";
    case StageCategory::Mixer:
        return "Mixer";
    case StageCategory::Processors:
        return "Processors";
    case StageCategory::Others:
        return "Others";
    }
    return "Filters";
}

std::string stageTypeToString(StageType type) {
    switch (type) {
    case StageType::Balance:
        return "Balance";
    case StageType::Width:
        return "Width";
    case StageType::MSProc:
        return "M/S Proc";
    case StageType::PhaseInvert:
        return "Phase Invert";
    case StageType::Crossfeed:
        return "Crossfeed";
    case StageType::SplitWidth:
        return "Split Width";
    case StageType::EQ:
        return "EQ";
    case StageType::GraphicEQ:
        return "Graphic EQ";
    case StageType::Convolution:
        return "Convolution";
    case StageType::Loudness:
        return "Loudness";
    case StageType::Emphasis:
        return "Emphasis";
    case StageType::DCProtection:
        return "DC Protection";
    case StageType::Gain:
        return "Gain";
    case StageType::Delay:
        return "Delay";
    case StageType::LookaheadLimiter:
        return "Lookahead Limiter";
    case StageType::Limiter:
        return "Limiter";
    case StageType::Volume:
        return "Volume";
    case StageType::MatrixMixer:
        return "Matrix Mixer";
    case StageType::Compressor:
        return "Compressor";
    case StageType::NoiseGate:
        return "Noise Gate";
    case StageType::RACE:
        return "RACE";
    case StageType::Dither:
        return "Dither";
    case StageType::DiffEq:
        return "Differential Equation";
    case StageType::BiquadCombo:
        return "Biquad Combo";
    }
    return "Gain";
}

StageCategory stageTypeToCategory(StageType type) {
    switch (type) {
    case StageType::EQ:
    case StageType::GraphicEQ:
    case StageType::Convolution:
    case StageType::BiquadCombo:
    case StageType::DiffEq:
    case StageType::Gain:
    case StageType::Delay:
    case StageType::Volume:
    case StageType::Limiter:
    case StageType::LookaheadLimiter:
    case StageType::Dither:
    case StageType::Loudness:
        return StageCategory::Filters;
    case StageType::MatrixMixer:
        return StageCategory::Mixer;
    case StageType::Compressor:
    case StageType::NoiseGate:
    case StageType::RACE:
        return StageCategory::Processors;
    case StageType::Balance:
    case StageType::Width:
    case StageType::MSProc:
    case StageType::PhaseInvert:
    case StageType::Crossfeed:
    case StageType::DCProtection:
    case StageType::Emphasis:
    case StageType::SplitWidth:
        return StageCategory::Others;
    }
    return StageCategory::Filters;
}

std::string stageTypeToIcon(StageType type) {
    switch (type) {
    case StageType::Balance:
        return "🎛️";
    case StageType::Width:
        return "↔️";
    case StageType::MSProc:
        return "🌊";
    case StageType::PhaseInvert:
        return "🔄";
    case StageType::Crossfeed:
        return "🎧";
    case StageType::SplitWidth:
        return "🔀";
    case StageType::EQ:
        return "🎚️";
    case StageType::GraphicEQ:
        return "🎛️";
    case StageType::Convolution:
        return "🌊";
    case StageType::Loudness:
        return "👂";
    case StageType::Emphasis:
        return "📈";
    case StageType::DCProtection:
        return "⚡";
    case StageType::Gain:
        return "➕";
    case StageType::Volume:
        return "🔊";
    case StageType::Delay:
        return "⏱️";
    case StageType::LookaheadLimiter:
        return "🧱";
    case StageType::MatrixMixer:
        return "🔳";
    case StageType::Compressor:
        return "🗜️";
    case StageType::NoiseGate:
        return "🚪";
    case StageType::RACE:
        return "🗣️";
    case StageType::Dither:
        return "🎲";
    case StageType::DiffEq:
        return "📐";
    case StageType::BiquadCombo:
        return "🎚️";
    case StageType::Limiter:
        return "✂️";
    }
    return "🎚️";
}

std::string crossfeedLevelToString(CrossfeedLevel l) {
    switch (l) {
    case CrossfeedLevel::Off:
        return "Off";
    case CrossfeedLevel::L1:
        return "L1";
    case CrossfeedLevel::L2:
        return "L2";
    case CrossfeedLevel::L3:
        return "L3";
    case CrossfeedLevel::L4:
        return "L4";
    case CrossfeedLevel::L5:
        return "L5";
    }
    return "Off";
}

CrossfeedLevel stringToCrossfeedLevel(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "l1" || s == "level1" || s == "1")
        return CrossfeedLevel::L1;
    if (s == "l2" || s == "level2" || s == "2")
        return CrossfeedLevel::L2;
    if (s == "l3" || s == "level3" || s == "3")
        return CrossfeedLevel::L3;
    if (s == "l4" || s == "level4" || s == "4")
        return CrossfeedLevel::L4;
    if (s == "l5" || s == "level5" || s == "5")
        return CrossfeedLevel::L5;
    if (s == "off" || s == "0")
        return CrossfeedLevel::Off;
    return CrossfeedLevel::L1;
}

std::string crossfeedLevelDescription(CrossfeedLevel l) {
    switch (l) {
    case CrossfeedLevel::Off:
        return "";
    case CrossfeedLevel::L1:
        return "Just a touch";
    case CrossfeedLevel::L2:
        return "Jan Meier";
    case CrossfeedLevel::L3:
        return "Chu Moy";
    case CrossfeedLevel::L4:
        return "30° 3m";
    case CrossfeedLevel::L5:
        return "Strong";
    }
    return "";
}

std::string emphasisModeToString(EmphasisMode m) {
    switch (m) {
    case EmphasisMode::Off:
        return "Off";
    case EmphasisMode::DeEmphasis:
        return "De-Emphasis";
    case EmphasisMode::PreEmphasis:
        return "Pre-Emphasis";
    }
    return "Off";
}

EmphasisMode stringToEmphasisMode(const std::string& str) {
    if (str == "De-Emphasis" || str == "deEmphasis")
        return EmphasisMode::DeEmphasis;
    if (str == "Pre-Emphasis" || str == "preEmphasis")
        return EmphasisMode::PreEmphasis;
    return EmphasisMode::Off;
}

std::string emphasisModeDescription(EmphasisMode m) {
    switch (m) {
    case EmphasisMode::Off:
        return "";
    case EmphasisMode::DeEmphasis:
        return "Highshelf at 5200 Hz, -9.5 dB, Q 0.5 (undo pre-emphasis)";
    case EmphasisMode::PreEmphasis:
        return "Highshelf at 5200 Hz, +9.5 dB, Q 0.5 (boost highs)";
    }
    return "";
}

PipelineStage::PipelineStage()
    : id(QUuid::createUuid()), type(StageType::Gain), name("Gain"), isEnabled(false), graphicEQGains(31, 0.0) {}

PipelineStage::PipelineStage(StageType type, const std::string& name, bool isEnabled, const std::vector<int>& channels)
    : id(QUuid::createUuid()), type(type), name(name.empty() ? stageTypeToString(type) : name), isEnabled(isEnabled),
      channels(channels), monitorChannels(channels), graphicEQGains(31, 0.0) {
    if (type == StageType::Balance || type == StageType::Width || type == StageType::MSProc ||
        type == StageType::Crossfeed || type == StageType::RACE || type == StageType::SplitWidth) {
        leftChannel = 0;
        rightChannel = 1;
    }

    if (type == StageType::MatrixMixer) {
        mixerChannelsIn = 2;
        mixerChannelsOut = 2;
        MixerMapping m0;
        m0.dest = 0;
        m0.sources.push_back(MixerSource{0, 0.0, false});
        MixerMapping m1;
        m1.dest = 1;
        m1.sources.push_back(MixerSource{1, 0.0, false});
        mixerMappings = {m0, m1};
    }
}

int PipelineStage::balanceLeftPercent() const {
    return static_cast<int>((1.0 - std::max(0.0, balancePosition)) * 100.0);
}

int PipelineStage::balanceRightPercent() const {
    return static_cast<int>((1.0 + std::min(0.0, balancePosition)) * 100.0);
}

int PipelineStage::widthPercent() const {
    return static_cast<int>(widthAmount * 100.0);
}

std::string PipelineStage::widthDescription() const {
    if (widthAmount == 1.0)
        return "Normal stereo (passthrough)";
    if (widthAmount == 0.0)
        return "Mono — L and R summed equally";
    if (widthAmount == -1.0)
        return "Fully swapped — L and R exchanged";
    if (widthAmount < 0.0)
        return "Partially swapped with crossfeed";
    if (widthAmount < 1.0)
        return "Narrowed stereo image";
    return "Enhanced stereo — wider than original";
}

bool PipelineStage::isActive() const {
    if (!isEnabled)
        return false;
    switch (type) {
    case StageType::Width:
        return widthAmount != 1.0;
    case StageType::Balance:
        return balancePosition != 0.0;
    case StageType::Crossfeed:
        return crossfeedLevel != CrossfeedLevel::Off;
    case StageType::Emphasis:
        return emphasisMode != EmphasisMode::Off;
    case StageType::Convolution:
        return convPresetId.has_value();
    case StageType::EQ:
        return eqPresetId.has_value();
    default:
        return true;
    }
}

CrossfeedParamsResult PipelineStage::computeCrossfeed(double fc, double db) {
    double gd = -5.0 * db / 6.0 - 3.0;
    double adH = db / 6.0 - 3.0;
    double aH = std::pow(10.0, adH / 20.0);
    double gH = 1.0 - aH;
    double gdH = 20.0 * std::log10(std::max(gH, 1e-10));
    double fcH = fc * std::pow(2.0, (gd - gdH) / 12.0) / std::pow(10.0, -adH / 80.0 / 0.5);
    return {fcH, adH, 0.5, fc, gd};
}

CrossfeedParamsResult PipelineStage::activeCrossfeedParams() const {
    if (cxCustomEnabled)
        return computeCrossfeed(cxFc, cxDb);
    double fc = 700.0;
    double db = 6.0;
    switch (crossfeedLevel) {
    case CrossfeedLevel::L1:
        fc = 650.0;
        db = 13.5;
        break;
    case CrossfeedLevel::L2:
        fc = 650.0;
        db = 9.5;
        break;
    case CrossfeedLevel::L3:
        fc = 700.0;
        db = 6.0;
        break;
    case CrossfeedLevel::L4:
        fc = 700.0;
        db = 4.5;
        break;
    case CrossfeedLevel::L5:
        fc = 700.0;
        db = 3.0;
        break;
    case CrossfeedLevel::Off:
        break;
    }
    return computeCrossfeed(fc, db);
}

QJsonObject PipelineStage::toJson() const {
    QJsonObject obj;
    obj["id"] = id.toString();
    obj["name"] = QString::fromStdString(name);
    obj["type"] = QString::fromStdString(stageTypeToString(type));
    obj["isEnabled"] = isEnabled;

    QJsonArray chArr;
    for (int c : channels)
        chArr.append(c);
    obj["channels"] = chArr;

    QJsonArray monArr;
    for (int c : monitorChannels)
        monArr.append(c);
    obj["monitorChannels"] = monArr;

    obj["leftChannel"] = leftChannel;
    obj["rightChannel"] = rightChannel;
    obj["balancePosition"] = balancePosition;
    obj["widthAmount"] = widthAmount;
    obj["crossfeedLevel"] = QString::fromStdString(crossfeedLevelToString(crossfeedLevel));
    obj["cxCustomEnabled"] = cxCustomEnabled;
    obj["cxFc"] = cxFc;
    obj["cxDb"] = cxDb;

    if (eqPresetId.has_value())
        obj["eqPresetID"] = eqPresetId.value().toString();
    if (convPresetId.has_value())
        obj["convPresetID"] = convPresetId.value().toString();

    obj["emphasisMode"] = QString::fromStdString(emphasisModeToString(emphasisMode));
    obj["loudnessReference"] = loudnessReference;
    obj["loudnessHighBoost"] = loudnessHighBoost;
    obj["loudnessLowBoost"] = loudnessLowBoost;
    obj["loudnessFader"] = static_cast<int>(loudnessFader);
    obj["loudnessAttenuateMid"] = loudnessAttenuateMid;

    obj["gainValue"] = gainValue;
    obj["gainInverted"] = gainInverted;
    obj["gainMuted"] = gainMuted;

    obj["volumeRampTime"] = volumeRampTime;
    obj["volumeLimit"] = volumeLimit;
    obj["volumeFader"] = static_cast<int>(volumeFader);

    obj["delayValue"] = delayValue;
    obj["delayUnit"] = QString::fromStdString(delayUnitToString(delayUnit));
    obj["delaySubsample"] = delaySubsample;

    obj["lookaheadLimit"] = lookaheadLimit;
    obj["lookaheadAttack"] = lookaheadAttack;
    obj["lookaheadRelease"] = lookaheadRelease;

    obj["mixerChannelsIn"] = mixerChannelsIn;
    obj["mixerChannelsOut"] = mixerChannelsOut;
    QJsonArray mapArr;
    for (const auto& m : mixerMappings)
        mapArr.append(m.toJson());
    obj["mixerMappings"] = mapArr;

    obj["compressorAttack"] = compressorAttack;
    obj["compressorRelease"] = compressorRelease;
    obj["compressorThreshold"] = compressorThreshold;
    obj["compressorRatio"] = compressorRatio;
    obj["compressorMakeupGain"] = compressorMakeupGain;
    obj["compressorSoftClip"] = compressorSoftClip;
    obj["compressorClipLimit"] = compressorClipLimit;

    obj["gateAttack"] = gateAttack;
    obj["gateRelease"] = gateRelease;
    obj["gateThreshold"] = gateThreshold;
    obj["gateAttenuation"] = gateAttenuation;

    obj["raceDelay"] = raceDelay;
    obj["raceAttenuation"] = raceAttenuation;
    obj["raceSubsampleDelay"] = raceSubsampleDelay;
    obj["raceDelayUnit"] = QString::fromStdString(delayUnitToString(raceDelayUnit));

    obj["ditherType"] = QString::fromStdString(ditherTypeToString(ditherType));
    obj["ditherBits"] = ditherBits;
    obj["ditherAmplitude"] = ditherAmplitude;

    obj["diffEqA"] = QString::fromStdString(diffEqA);
    obj["diffEqB"] = QString::fromStdString(diffEqB);

    obj["comboType"] = QString::fromStdString(biquadComboTypeToString(comboType));
    obj["comboFreq"] = comboFreq;
    obj["comboOrder"] = comboOrder;
    obj["comboGain"] = comboGain;
    obj["comboGains"] = QString::fromStdString(comboGains);
    obj["comboFreqMin"] = comboFreqMin;
    obj["comboFreqMax"] = comboFreqMax;

    obj["peqFls"] = peqFls;
    obj["peqGls"] = peqGls;
    obj["peqQls"] = peqQls;
    obj["peqF1"] = peqF1;
    obj["peqG1"] = peqG1;
    obj["peqQ1"] = peqQ1;
    obj["peqF2"] = peqF2;
    obj["peqG2"] = peqG2;
    obj["peqQ2"] = peqQ2;
    obj["peqF3"] = peqF3;
    obj["peqG3"] = peqG3;
    obj["peqQ3"] = peqQ3;
    obj["peqFhs"] = peqFhs;
    obj["peqGhs"] = peqGhs;
    obj["peqQhs"] = peqQhs;

    obj["limiterLimit"] = limiterLimit;
    obj["limiterSoftClip"] = limiterSoftClip;

    obj["splitWidthCrossover"] = splitWidthCrossover;
    obj["splitWidthAmount"] = splitWidthAmount;

    obj["graphicEQFreqMin"] = graphicEQFreqMin;
    obj["graphicEQFreqMax"] = graphicEQFreqMax;
    obj["graphicEQBandCount"] = graphicEQBandCount;
    QJsonArray geqArr;
    for (double g : graphicEQGains)
        geqArr.append(g);
    obj["graphicEQGains"] = geqArr;

    return obj;
}

PipelineStage PipelineStage::fromJson(const QJsonObject& json) {
    PipelineStage s;
    if (json.contains("id"))
        s.id = QUuid::fromString(json["id"].toString());
    if (json.contains("name"))
        s.name = json["name"].toString().toStdString();
    if (json.contains("type")) {
        std::string typeStr = json["type"].toString().toStdString();
        QString cleanInput = QString::fromStdString(typeStr).remove(" ").toLower();
        for (StageType st : {StageType::Balance,     StageType::Width,     StageType::MSProc,
                             StageType::PhaseInvert, StageType::Crossfeed, StageType::SplitWidth,
                             StageType::EQ,          StageType::GraphicEQ, StageType::Convolution,
                             StageType::Loudness,    StageType::Emphasis,  StageType::DCProtection,
                             StageType::Gain,        StageType::Delay,     StageType::LookaheadLimiter,
                             StageType::Limiter,     StageType::Volume,    StageType::MatrixMixer,
                             StageType::Compressor,  StageType::NoiseGate, StageType::RACE,
                             StageType::Dither,      StageType::DiffEq,    StageType::BiquadCombo}) {
            QString targetStr = QString::fromStdString(stageTypeToString(st)).remove(" ").toLower();
            if (stageTypeToString(st) == typeStr || targetStr == cleanInput) {
                s.type = st;
                break;
            }
        }
    }
    if (json.contains("isEnabled"))
        s.isEnabled = json["isEnabled"].toBool();
    if (json.contains("channels")) {
        s.channels.clear();
        for (const auto& c : json["channels"].toArray())
            s.channels.push_back(c.toInt());
    }
    if (json.contains("monitorChannels")) {
        s.monitorChannels.clear();
        for (const auto& c : json["monitorChannels"].toArray())
            s.monitorChannels.push_back(c.toInt());
    }
    if (json.contains("leftChannel"))
        s.leftChannel = json["leftChannel"].toInt();
    if (json.contains("rightChannel"))
        s.rightChannel = json["rightChannel"].toInt();
    if (json.contains("balancePosition"))
        s.balancePosition = json["balancePosition"].toDouble();
    if (json.contains("widthAmount"))
        s.widthAmount = json["widthAmount"].toDouble();
    if (json.contains("crossfeedLevel"))
        s.crossfeedLevel = stringToCrossfeedLevel(json["crossfeedLevel"].toString().toStdString());
    if (json.contains("cxCustomEnabled"))
        s.cxCustomEnabled = json["cxCustomEnabled"].toBool();
    if (json.contains("cxFc"))
        s.cxFc = json["cxFc"].toDouble();
    if (json.contains("cxDb"))
        s.cxDb = json["cxDb"].toDouble();

    if (json.contains("eqPresetID"))
        s.eqPresetId = QUuid::fromString(json["eqPresetID"].toString());
    else if (json.contains("eqPresetId"))
        s.eqPresetId = QUuid::fromString(json["eqPresetId"].toString());

    if (json.contains("convPresetID"))
        s.convPresetId = QUuid::fromString(json["convPresetID"].toString());
    else if (json.contains("convPresetId"))
        s.convPresetId = QUuid::fromString(json["convPresetId"].toString());

    if (json.contains("emphasisMode"))
        s.emphasisMode = stringToEmphasisMode(json["emphasisMode"].toString().toStdString());
    if (json.contains("loudnessReference"))
        s.loudnessReference = json["loudnessReference"].toDouble();
    if (json.contains("loudnessHighBoost"))
        s.loudnessHighBoost = json["loudnessHighBoost"].toDouble();
    if (json.contains("loudnessLowBoost"))
        s.loudnessLowBoost = json["loudnessLowBoost"].toDouble();
    if (json.contains("loudnessFader"))
        s.loudnessFader = static_cast<Fader>(json["loudnessFader"].toInt());
    if (json.contains("loudnessAttenuateMid"))
        s.loudnessAttenuateMid = json["loudnessAttenuateMid"].toBool();

    if (json.contains("gainValue"))
        s.gainValue = json["gainValue"].toDouble();
    if (json.contains("gainInverted"))
        s.gainInverted = json["gainInverted"].toBool();
    if (json.contains("gainMuted"))
        s.gainMuted = json["gainMuted"].toBool();

    if (json.contains("volumeRampTime"))
        s.volumeRampTime = json["volumeRampTime"].toDouble();
    if (json.contains("volumeLimit"))
        s.volumeLimit = json["volumeLimit"].toDouble();
    if (json.contains("volumeFader"))
        s.volumeFader = static_cast<Fader>(json["volumeFader"].toInt());

    if (json.contains("delayValue"))
        s.delayValue = json["delayValue"].toDouble();
    if (json.contains("delayUnit"))
        s.delayUnit = stringToDelayUnit(json["delayUnit"].toString().toStdString());
    if (json.contains("delaySubsample"))
        s.delaySubsample = json["delaySubsample"].toBool();

    if (json.contains("lookaheadLimit"))
        s.lookaheadLimit = json["lookaheadLimit"].toDouble();
    if (json.contains("lookaheadAttack"))
        s.lookaheadAttack = json["lookaheadAttack"].toDouble();
    if (json.contains("lookaheadRelease"))
        s.lookaheadRelease = json["lookaheadRelease"].toDouble();

    if (json.contains("mixerChannelsIn"))
        s.mixerChannelsIn = json["mixerChannelsIn"].toInt();
    if (json.contains("mixerChannelsOut"))
        s.mixerChannelsOut = json["mixerChannelsOut"].toInt();
    if (json.contains("mixerMappings")) {
        s.mixerMappings.clear();
        for (const auto& v : json["mixerMappings"].toArray()) {
            s.mixerMappings.push_back(MixerMapping::fromJson(v.toObject()));
        }
    }

    if (json.contains("compressorAttack"))
        s.compressorAttack = json["compressorAttack"].toDouble();
    if (json.contains("compressorRelease"))
        s.compressorRelease = json["compressorRelease"].toDouble();
    if (json.contains("compressorThreshold"))
        s.compressorThreshold = json["compressorThreshold"].toDouble();
    if (json.contains("compressorRatio"))
        s.compressorRatio = json["compressorRatio"].toDouble();
    if (json.contains("compressorMakeupGain"))
        s.compressorMakeupGain = json["compressorMakeupGain"].toDouble();
    if (json.contains("compressorSoftClip"))
        s.compressorSoftClip = json["compressorSoftClip"].toBool();
    if (json.contains("compressorClipLimit"))
        s.compressorClipLimit = json["compressorClipLimit"].toDouble();

    if (json.contains("gateAttack"))
        s.gateAttack = json["gateAttack"].toDouble();
    if (json.contains("gateRelease"))
        s.gateRelease = json["gateRelease"].toDouble();
    if (json.contains("gateThreshold"))
        s.gateThreshold = json["gateThreshold"].toDouble();
    if (json.contains("gateAttenuation"))
        s.gateAttenuation = json["gateAttenuation"].toDouble();

    if (json.contains("raceDelay"))
        s.raceDelay = json["raceDelay"].toDouble();
    if (json.contains("raceAttenuation"))
        s.raceAttenuation = json["raceAttenuation"].toDouble();
    if (json.contains("raceSubsampleDelay"))
        s.raceSubsampleDelay = json["raceSubsampleDelay"].toBool();
    if (json.contains("raceDelayUnit"))
        s.raceDelayUnit = stringToDelayUnit(json["raceDelayUnit"].toString().toStdString());

    if (json.contains("ditherType"))
        s.ditherType = stringToDitherType(json["ditherType"].toString().toStdString());
    if (json.contains("ditherBits"))
        s.ditherBits = json["ditherBits"].toInt();
    if (json.contains("ditherAmplitude"))
        s.ditherAmplitude = json["ditherAmplitude"].toDouble();

    if (json.contains("diffEqA"))
        s.diffEqA = json["diffEqA"].toString().toStdString();
    if (json.contains("diffEqB"))
        s.diffEqB = json["diffEqB"].toString().toStdString();

    if (json.contains("comboType"))
        s.comboType = stringToBiquadComboType(json["comboType"].toString().toStdString());
    if (json.contains("comboFreq"))
        s.comboFreq = json["comboFreq"].toDouble();
    if (json.contains("comboOrder"))
        s.comboOrder = json["comboOrder"].toInt();
    if (json.contains("comboGain"))
        s.comboGain = json["comboGain"].toDouble();
    if (json.contains("comboGains"))
        s.comboGains = json["comboGains"].toString().toStdString();
    if (json.contains("comboFreqMin"))
        s.comboFreqMin = json["comboFreqMin"].toDouble();
    if (json.contains("comboFreqMax"))
        s.comboFreqMax = json["comboFreqMax"].toDouble();

    if (json.contains("peqFls"))
        s.peqFls = json["peqFls"].toDouble();
    if (json.contains("peqGls"))
        s.peqGls = json["peqGls"].toDouble();
    if (json.contains("peqQls"))
        s.peqQls = json["peqQls"].toDouble();
    if (json.contains("peqF1"))
        s.peqF1 = json["peqF1"].toDouble();
    if (json.contains("peqG1"))
        s.peqG1 = json["peqG1"].toDouble();
    if (json.contains("peqQ1"))
        s.peqQ1 = json["peqQ1"].toDouble();
    if (json.contains("peqF2"))
        s.peqF2 = json["peqF2"].toDouble();
    if (json.contains("peqG2"))
        s.peqG2 = json["peqG2"].toDouble();
    if (json.contains("peqQ2"))
        s.peqQ2 = json["peqQ2"].toDouble();
    if (json.contains("peqF3"))
        s.peqF3 = json["peqF3"].toDouble();
    if (json.contains("peqG3"))
        s.peqG3 = json["peqG3"].toDouble();
    if (json.contains("peqQ3"))
        s.peqQ3 = json["peqQ3"].toDouble();
    if (json.contains("peqFhs"))
        s.peqFhs = json["peqFhs"].toDouble();
    if (json.contains("peqGhs"))
        s.peqGhs = json["peqGhs"].toDouble();
    if (json.contains("peqQhs"))
        s.peqQhs = json["peqQhs"].toDouble();

    if (json.contains("limiterLimit"))
        s.limiterLimit = json["limiterLimit"].toDouble();
    if (json.contains("limiterSoftClip"))
        s.limiterSoftClip = json["limiterSoftClip"].toBool();

    if (json.contains("splitWidthCrossover"))
        s.splitWidthCrossover = json["splitWidthCrossover"].toDouble();
    if (json.contains("splitWidthAmount"))
        s.splitWidthAmount = json["splitWidthAmount"].toDouble();

    if (json.contains("graphicEQFreqMin"))
        s.graphicEQFreqMin = json["graphicEQFreqMin"].toDouble();
    if (json.contains("graphicEQFreqMax"))
        s.graphicEQFreqMax = json["graphicEQFreqMax"].toDouble();
    if (json.contains("graphicEQBandCount"))
        s.graphicEQBandCount = json["graphicEQBandCount"].toInt();
    if (json.contains("graphicEQGains")) {
        s.graphicEQGains.clear();
        for (const auto& g : json["graphicEQGains"].toArray())
            s.graphicEQGains.push_back(g.toDouble());
    }

    return s;
}

bool PipelineStage::operator==(const PipelineStage& other) const {
    return id == other.id;
}

StageBuildResult StageBuilders::buildStage(const PipelineStage& stage, int sampleRate, int channelCount,
                                           const std::map<QUuid, EQPreset>& eqPresets,
                                           const std::map<QUuid, ConvolutionPreset>& convPresets) {
    StageBuildResult res;
    if (!stage.isActive())
        return res;

    int leftCh = (stage.leftChannel < channelCount) ? stage.leftChannel : 0;
    int rightCh = (stage.rightChannel < channelCount && stage.rightChannel != leftCh)
                      ? stage.rightChannel
                      : (channelCount > 1 ? 1 : 0);

    if (stage.type == StageType::Balance || stage.type == StageType::Width || stage.type == StageType::MSProc ||
        stage.type == StageType::Crossfeed || stage.type == StageType::RACE || stage.type == StageType::SplitWidth) {
        if (leftCh >= channelCount || rightCh >= channelCount) {
            return res;
        }
    }

    std::string prefix = QString::fromStdString(stageTypeToString(stage.type)).toLower().toStdString() + "_" +
                         stage.id.toString(QUuid::WithoutBraces).left(8).toStdString();
    std::vector<int> chList;
    for (int c : stage.channels) {
        if (c < channelCount) {
            chList.push_back(c);
        }
    }
    std::sort(chList.begin(), chList.end());

    std::vector<int> monitorList;
    for (int c : stage.monitorChannels) {
        if (c < channelCount) {
            monitorList.push_back(c);
        }
    }
    std::sort(monitorList.begin(), monitorList.end());
    if (monitorList.empty()) {
        monitorList = chList;
    }

    switch (stage.type) {
    case StageType::Balance: {
        double leftLin = 1.0 - std::max(0.0, stage.balancePosition);
        double rightLin = 1.0 + std::min(0.0, stage.balancePosition);
        double leftDB = leftLin > 0.0 ? 20.0 * std::log10(leftLin) : -100.0;
        double rightDB = rightLin > 0.0 ? 20.0 * std::log10(rightLin) : -100.0;

        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int i = 0; i < channelCount; ++i) {
            MixerMapping m;
            m.dest = i;
            if (i == leftCh) {
                m.sources.push_back(MixerSource{leftCh, leftDB, false});
            } else if (i == rightCh) {
                m.sources.push_back(MixerSource{rightCh, rightDB, false});
            } else {
                m.sources.push_back(MixerSource{i, 0.0, false});
            }
            mc.mapping.push_back(m);
        }
        res.mixers[prefix] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = prefix;
        res.steps.push_back(step);
        break;
    }

    case StageType::Width: {
        double w = stage.widthAmount;
        double ll = (1.0 + w) / 2.0;
        double lr = (1.0 - w) / 2.0;
        double threshold = 1e-6;

        auto makeSources = [leftCh, rightCh, threshold](double ch0, double ch1) {
            std::vector<MixerSource> sources;
            if (std::abs(ch0) > threshold) {
                sources.push_back(MixerSource{leftCh, 20.0 * std::log10(std::abs(ch0)), ch0 < 0});
            }
            if (std::abs(ch1) > threshold) {
                sources.push_back(MixerSource{rightCh, 20.0 * std::log10(std::abs(ch1)), ch1 < 0});
            }
            return sources;
        };

        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int i = 0; i < channelCount; ++i) {
            MixerMapping m;
            m.dest = i;
            if (i == leftCh) {
                m.sources = makeSources(ll, lr);
            } else if (i == rightCh) {
                m.sources = makeSources(lr, ll);
            } else {
                m.sources.push_back(MixerSource{i, 0.0, false});
            }
            mc.mapping.push_back(m);
        }
        res.mixers[prefix] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = prefix;
        res.steps.push_back(step);
        break;
    }

    case StageType::MSProc: {
        MixerConfig mc;
        mc.channelsIn = channelCount;
        mc.channelsOut = channelCount;
        for (int i = 0; i < channelCount; ++i) {
            MixerMapping m;
            m.dest = i;
            if (i == leftCh) {
                m.sources.push_back(MixerSource{leftCh, -6.02, false});
                m.sources.push_back(MixerSource{rightCh, -6.02, false});
            } else if (i == rightCh) {
                m.sources.push_back(MixerSource{leftCh, -6.02, false});
                m.sources.push_back(MixerSource{rightCh, -6.02, true});
            } else {
                m.sources.push_back(MixerSource{i, 0.0, false});
            }
            mc.mapping.push_back(m);
        }
        res.mixers[prefix] = mc;

        PipelineStep step;
        step.type = PipelineStepType::Mixer;
        step.name = prefix;
        res.steps.push_back(step);
        break;
    }

    case StageType::PhaseInvert: {
        FilterConfig f;
        f.type = FilterType::Gain;
        f.gainParams.gain = 0.0;
        f.gainParams.inverted = true;
        std::string filterKey = prefix + "_invert";
        res.filters[filterKey] = f;

        if (!chList.empty()) {
            PipelineStep step;
            step.type = PipelineStepType::Filter;
            step.channels = chList;
            step.names.push_back(filterKey);
            res.steps.push_back(step);
        }
        break;
    }

    case StageType::Crossfeed: {
        if ((stage.crossfeedLevel == CrossfeedLevel::Off && !stage.cxCustomEnabled) || channelCount < 2)
            break;
        auto cx = stage.activeCrossfeedParams();

        FilterConfig fHi, fLo, fLoGain;
        fHi.type = FilterType::Biquad;
        fHi.biquadParams.type = BiquadType::Lowshelf;
        fHi.biquadParams.freq = cx.hiFreq;
        fHi.biquadParams.gain = cx.hiGain;
        fHi.biquadParams.q = cx.hiQ;

        fLo.type = FilterType::Biquad;
        fLo.biquadParams.type = BiquadType::LowpassFO;
        fLo.biquadParams.freq = cx.loFreq;

        fLoGain.type = FilterType::Gain;
        fLoGain.gainParams.gain = cx.loGain;
        fLoGain.gainParams.inverted = false;

        res.filters[prefix + "_hi"] = fHi;
        res.filters[prefix + "_lo"] = fLo;
        res.filters[prefix + "_lo_gain"] = fLoGain;

        int leftCh = 0;
        int rightCh = 1;

        std::vector<int> otherChannels;
        for (int i = 0; i < channelCount; ++i) {
            if (i != leftCh && i != rightCh) {
                otherChannels.push_back(i);
            }
        }

        MixerConfig m2to4;
        m2to4.channelsIn = channelCount;
        m2to4.channelsOut = channelCount + 2;
        m2to4.mapping.push_back(MixerMapping{0, {MixerSource{leftCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{1, {MixerSource{leftCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{2, {MixerSource{rightCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{3, {MixerSource{rightCh, 0.0, false}}});
        for (size_t idx = 0; idx < otherChannels.size(); ++idx) {
            m2to4.mapping.push_back(
                MixerMapping{static_cast<int>(idx + 4), {MixerSource{otherChannels[idx], 0.0, false}}});
        }
        res.mixers[prefix + "_2to4"] = m2to4;

        MixerConfig m4to2;
        m4to2.channelsIn = channelCount + 2;
        m4to2.channelsOut = channelCount;
        m4to2.mapping.resize(channelCount);
        m4to2.mapping[leftCh] = MixerMapping{leftCh, {MixerSource{0, 0.0, false}, MixerSource{2, 0.0, false}}};
        m4to2.mapping[rightCh] = MixerMapping{rightCh, {MixerSource{1, 0.0, false}, MixerSource{3, 0.0, false}}};
        for (size_t idx = 0; idx < otherChannels.size(); ++idx) {
            int ch = otherChannels[idx];
            m4to2.mapping[ch] = MixerMapping{ch, {MixerSource{static_cast<int>(idx + 4), 0.0, false}}};
        }
        res.mixers[prefix + "_4to2"] = m4to2;

        res.steps.push_back(
            PipelineStep{PipelineStepType::Mixer, std::nullopt, {}, prefix + "_2to4", {}, std::nullopt});
        res.steps.push_back(
            PipelineStep{PipelineStepType::Filter, std::nullopt, {0, 3}, std::nullopt, {prefix + "_hi"}, std::nullopt});
        res.steps.push_back(PipelineStep{PipelineStepType::Filter,
                                         std::nullopt,
                                         {1, 2},
                                         std::nullopt,
                                         {prefix + "_lo", prefix + "_lo_gain"},
                                         std::nullopt});
        res.steps.push_back(
            PipelineStep{PipelineStepType::Mixer, std::nullopt, {}, prefix + "_4to2", {}, std::nullopt});
        break;
    }

    case StageType::SplitWidth: {
        if (channelCount < 2)
            break;
        FilterConfig fLp, fHp;
        fLp.type = FilterType::BiquadCombo;
        fLp.comboParams.type = BiquadComboType::LinkwitzRileyLowpass;
        fLp.comboParams.freq = stage.splitWidthCrossover;
        fLp.comboParams.order = 4;

        fHp.type = FilterType::BiquadCombo;
        fHp.comboParams.type = BiquadComboType::LinkwitzRileyHighpass;
        fHp.comboParams.freq = stage.splitWidthCrossover;
        fHp.comboParams.order = 4;

        res.filters[prefix + "_lp"] = fLp;
        res.filters[prefix + "_hp"] = fHp;

        std::vector<int> otherChannels;
        for (int i = 0; i < channelCount; ++i) {
            if (i != leftCh && i != rightCh) {
                otherChannels.push_back(i);
            }
        }

        MixerConfig m2to4;
        m2to4.channelsIn = channelCount;
        m2to4.channelsOut = channelCount + 2;
        m2to4.mapping.push_back(MixerMapping{0, {MixerSource{leftCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{1, {MixerSource{rightCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{2, {MixerSource{leftCh, 0.0, false}}});
        m2to4.mapping.push_back(MixerMapping{3, {MixerSource{rightCh, 0.0, false}}});
        for (size_t idx = 0; idx < otherChannels.size(); ++idx) {
            m2to4.mapping.push_back(
                MixerMapping{static_cast<int>(idx + 4), {MixerSource{otherChannels[idx], 0.0, false}}});
        }
        res.mixers[prefix + "_2to4"] = m2to4;

        double c1 = 0.5 * (1.0 + stage.splitWidthAmount);
        double c2 = 0.5 * (1.0 - stage.splitWidthAmount);

        auto makeMixerSource = [](int ch, double linearGain) {
            double absGain = std::abs(linearGain);
            double gainDb = absGain > 1e-5 ? 20.0 * std::log10(absGain) : -120.0;
            bool isInverted = linearGain < 0.0;
            return MixerSource{ch, gainDb, isInverted};
        };

        MixerConfig m4to2;
        m4to2.channelsIn = channelCount + 2;
        m4to2.channelsOut = channelCount;
        m4to2.mapping.resize(channelCount);
        m4to2.mapping[leftCh] = MixerMapping{
            leftCh, {MixerSource{0, 0.0, false}, makeMixerSource(2, c1), makeMixerSource(3, c2)}};
        m4to2.mapping[rightCh] = MixerMapping{
            rightCh, {MixerSource{1, 0.0, false}, makeMixerSource(2, c2), makeMixerSource(3, c1)}};
        for (size_t idx = 0; idx < otherChannels.size(); ++idx) {
            int ch = otherChannels[idx];
            m4to2.mapping[ch] = MixerMapping{ch, {MixerSource{static_cast<int>(idx + 4), 0.0, false}}};
        }
        res.mixers[prefix + "_4to2"] = m4to2;

        res.steps.push_back(
            PipelineStep{PipelineStepType::Mixer, std::nullopt, {}, prefix + "_2to4", {}, std::nullopt});
        res.steps.push_back(
            PipelineStep{PipelineStepType::Filter, std::nullopt, {0, 1}, std::nullopt, {prefix + "_lp"}, std::nullopt});
        res.steps.push_back(
            PipelineStep{PipelineStepType::Filter, std::nullopt, {2, 3}, std::nullopt, {prefix + "_hp"}, std::nullopt});
        res.steps.push_back(
            PipelineStep{PipelineStepType::Mixer, std::nullopt, {}, prefix + "_4to2", {}, std::nullopt});
        break;
    }

    case StageType::EQ: {
        if (!stage.eqPresetId.has_value() || !eqPresets.count(stage.eqPresetId.value()) || chList.empty())
            break;
        const auto& preset = eqPresets.at(stage.eqPresetId.value());

        std::vector<std::string> names;
        FilterConfig pGain;
        pGain.type = FilterType::Gain;
        pGain.gainParams.gain = preset.preampGain;
        pGain.gainParams.inverted = false;
        res.filters[prefix + "_preamp"] = pGain;
        names.push_back(prefix + "_preamp");

        for (size_t i = 0; i < preset.bands.size(); ++i) {
            const auto& band = preset.bands[i];
            if (!band.isEnabled)
                continue;

            FilterConfig f;
            f.type = FilterType::Biquad;
            f.biquadParams.type = stringToBiquadType(eqBandTypeToString(band.type));

            switch (band.type) {
            case EQBandType::Free:
                f.biquadParams.b0 = band.b0;
                f.biquadParams.b1 = band.b1;
                f.biquadParams.b2 = band.b2;
                f.biquadParams.a1 = band.a1;
                f.biquadParams.a2 = band.a2;
                break;
            case EQBandType::GeneralNotch:
                f.biquadParams.freqNotch = band.freqNotch;
                f.biquadParams.freqPole = band.freqPole;
                f.biquadParams.qP = band.qPole;
                f.biquadParams.normalizeAtDc = band.normalizeAtDc;
                break;
            case EQBandType::LinkwitzTransform:
                f.biquadParams.freqAct = band.freqAct;
                f.biquadParams.qAct = band.qAct;
                f.biquadParams.freqTarget = band.freqTarget;
                f.biquadParams.qTarget = band.qTarget;
                break;
            case EQBandType::Lowshelf:
            case EQBandType::Highshelf:
                f.biquadParams.freq = band.freq;
                f.biquadParams.gain = band.gain;
                if (band.useSlope)
                    f.biquadParams.slope = band.slope;
                else
                    f.biquadParams.q = band.q;
                break;
            case EQBandType::Notch:
            case EQBandType::Bandpass:
            case EQBandType::Allpass:
                f.biquadParams.freq = band.freq;
                if (band.useBandwidth)
                    f.biquadParams.bandwidth = band.bandwidth;
                else
                    f.biquadParams.q = band.q;
                break;
            default:
                f.biquadParams.freq = band.freq;
                if (eqBandTypeHasGain(band.type))
                    f.biquadParams.gain = band.gain;
                if (eqBandTypeHasQ(band.type))
                    f.biquadParams.q = band.q;
                break;
            }

            std::string bKey = prefix + "_" + std::to_string(i + 1);
            res.filters[bKey] = f;
            names.push_back(bKey);
        }

        res.steps.push_back(
            PipelineStep{PipelineStepType::Filter, std::nullopt, chList, std::nullopt, names, std::nullopt});
        break;
    }

    case StageType::Convolution: {
        if (!stage.convPresetId.has_value() || !convPresets.count(stage.convPresetId.value()) || chList.empty())
            break;
        const auto& preset = convPresets.at(stage.convPresetId.value());
        std::string path = preset.irPath(sampleRate);
        if (path.empty())
            break;

        FilterConfig f;
        f.type = FilterType::Conv;
        f.convParams.type = ConvType::Raw;
        f.convParams.filename = path;
        f.convParams.format = "F64_LE";
        res.filters[prefix + "_conv"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_conv"}, std::nullopt});
        break;
    }

    case StageType::Loudness: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Loudness;
        f.loudnessParams.referenceLevel = stage.loudnessReference;
        f.loudnessParams.highBoost = stage.loudnessHighBoost;
        f.loudnessParams.lowBoost = stage.loudnessLowBoost;
        f.loudnessParams.attenuateMid = stage.loudnessAttenuateMid;
        f.loudnessParams.fader = stage.loudnessFader;
        res.filters[prefix + "_loudness"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_loudness"}, std::nullopt});
        break;
    }

    case StageType::Emphasis: {
        if (chList.empty() || stage.emphasisMode == EmphasisMode::Off)
            break;
        FilterConfig f;
        f.type = FilterType::Biquad;
        f.biquadParams.type = BiquadType::Highshelf;
        f.biquadParams.freq = 5200.0;
        f.biquadParams.gain = (stage.emphasisMode == EmphasisMode::DeEmphasis) ? -9.5 : 9.5;
        f.biquadParams.q = 0.5;

        std::string fKey = prefix + ((stage.emphasisMode == EmphasisMode::DeEmphasis) ? "_deemphasis" : "_preemphasis");
        res.filters[fKey] = f;

        res.steps.push_back(
            PipelineStep{PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {fKey}, std::nullopt});
        break;
    }

    case StageType::DCProtection: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Biquad;
        f.biquadParams.type = BiquadType::HighpassFO;
        f.biquadParams.freq = 7.0;
        res.filters[prefix + "_dcp"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_dcp"}, std::nullopt});
        break;
    }

    case StageType::Gain: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Gain;
        f.gainParams.gain = stage.gainValue;
        f.gainParams.scale = GainScale::dB;
        f.gainParams.inverted = stage.gainInverted;
        f.gainParams.mute = stage.gainMuted;
        res.filters[prefix + "_gain"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_gain"}, std::nullopt});
        break;
    }

    case StageType::Delay: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Delay;
        f.delayParams.delay = stage.delayValue;
        f.delayParams.unit = stage.delayUnit;
        f.delayParams.subsample = stage.delaySubsample;
        res.filters[prefix + "_delay"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_delay"}, std::nullopt});
        break;
    }

    case StageType::Volume: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Volume;
        f.volumeParams.rampTime = stage.volumeRampTime;
        f.volumeParams.limit = stage.volumeLimit;
        f.volumeParams.fader = stage.volumeFader;
        res.filters[prefix + "_volume"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_volume"}, std::nullopt});
        break;
    }

    case StageType::LookaheadLimiter: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::LookaheadLimiter;
        f.lookaheadParams.limit = stage.lookaheadLimit;
        f.lookaheadParams.attack = stage.lookaheadAttack;
        f.lookaheadParams.release = stage.lookaheadRelease;
        f.lookaheadParams.unit = DelayUnit::ms;
        res.filters[prefix + "_lookahead_limiter"] = f;

        res.steps.push_back(PipelineStep{PipelineStepType::Filter,
                                         std::nullopt,
                                         chList,
                                         std::nullopt,
                                         {prefix + "_lookahead_limiter"},
                                         std::nullopt});
        break;
    }

    case StageType::MatrixMixer: {
        std::vector<MixerMapping> cleanedMapping;
        for (int dest = 0; dest < stage.mixerChannelsOut; ++dest) {
            auto it = std::find_if(stage.mixerMappings.begin(), stage.mixerMappings.end(),
                                   [dest](const MixerMapping& m) { return m.dest == dest; });
            if (it != stage.mixerMappings.end()) {
                std::vector<MixerSource> cleanedSources;
                for (const auto& src : it->sources) {
                    if (src.channel < channelCount) {
                        cleanedSources.push_back(src);
                    }
                }
                cleanedMapping.push_back(MixerMapping{dest, cleanedSources, it->mute});
            } else {
                int src = dest < channelCount ? dest : 0;
                cleanedMapping.push_back(MixerMapping{dest, {MixerSource{src, 0.0, false}}, std::nullopt});
            }
        }

        res.mixers[prefix] = MixerConfig{channelCount, stage.mixerChannelsOut, cleanedMapping, std::nullopt, {}};
        res.steps.push_back(PipelineStep{PipelineStepType::Mixer, std::nullopt, {}, prefix, {}, std::nullopt});
        break;
    }

    case StageType::Compressor: {
        if (chList.empty())
            break;
        ProcessorConfig p;
        p.type = ProcessorType::Compressor;
        p.compressorParams.channels = channelCount;
        p.compressorParams.monitorChannels = monitorList;
        p.compressorParams.processChannels = chList;
        p.compressorParams.attack = stage.compressorAttack;
        p.compressorParams.release = stage.compressorRelease;
        p.compressorParams.threshold = stage.compressorThreshold;
        p.compressorParams.factor = stage.compressorRatio;
        p.compressorParams.makeupGain = stage.compressorMakeupGain;
        p.compressorParams.softClip = stage.compressorSoftClip;
        p.compressorParams.clipLimit = stage.compressorClipLimit;
        res.processors[prefix] = p;

        res.steps.push_back(PipelineStep{PipelineStepType::Processor, std::nullopt, {}, prefix, {}, std::nullopt});
        break;
    }

    case StageType::NoiseGate: {
        if (chList.empty())
            break;
        ProcessorConfig p;
        p.type = ProcessorType::NoiseGate;
        p.noiseGateParams.channels = channelCount;
        p.noiseGateParams.monitorChannels = monitorList;
        p.noiseGateParams.processChannels = chList;
        p.noiseGateParams.attack = stage.gateAttack;
        p.noiseGateParams.release = stage.gateRelease;
        p.noiseGateParams.threshold = stage.gateThreshold;
        p.noiseGateParams.attenuation = stage.gateAttenuation;
        res.processors[prefix] = p;

        res.steps.push_back(PipelineStep{PipelineStepType::Processor, std::nullopt, {}, prefix, {}, std::nullopt});
        break;
    }

    case StageType::RACE: {
        if (leftCh >= channelCount || rightCh >= channelCount)
            break;
        ProcessorConfig p;
        p.type = ProcessorType::RACE;
        p.raceParams.channels = channelCount;
        p.raceParams.channelA = leftCh;
        p.raceParams.channelB = rightCh;
        p.raceParams.delay = stage.raceDelay;
        p.raceParams.subsampleDelay = stage.raceSubsampleDelay;
        p.raceParams.delayUnit = stage.raceDelayUnit;
        p.raceParams.attenuation = stage.raceAttenuation;
        res.processors[prefix] = p;

        res.steps.push_back(PipelineStep{PipelineStepType::Processor, std::nullopt, {}, prefix, {}, std::nullopt});
        break;
    }

    case StageType::Dither: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Dither;
        f.ditherParams.type = stage.ditherType;
        f.ditherParams.bits = stage.ditherBits;
        f.ditherParams.amplitude = stage.ditherAmplitude;
        res.filters[prefix + "_dither"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_dither"}, std::nullopt});
        break;
    }

    case StageType::DiffEq: {
        if (chList.empty())
            break;
        auto stringToDoubleVec = [](const std::string& str) {
            std::vector<double> resVec;
            size_t start = 0;
            while (start < str.size()) {
                size_t end = str.find(',', start);
                if (end == std::string::npos)
                    end = str.size();
                std::string token = str.substr(start, end - start);
                token.erase(0, token.find_first_not_of(" \t\n\r"));
                token.erase(token.find_last_not_of(" \t\n\r") + 1);
                if (!token.empty()) {
                    bool ok = false;
                    double val = QString::fromStdString(token).toDouble(&ok);
                    if (ok) {
                        resVec.push_back(val);
                    }
                }
                start = end + 1;
            }
            return resVec;
        };

        FilterConfig f;
        f.type = FilterType::DiffEq;
        f.diffEqParams.a = stringToDoubleVec(stage.diffEqA);
        f.diffEqParams.b = stringToDoubleVec(stage.diffEqB);
        res.filters[prefix + "_diffeq"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_diffeq"}, std::nullopt});
        break;
    }

    case StageType::BiquadCombo: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::BiquadCombo;
        f.comboParams.type = stage.comboType;
        switch (stage.comboType) {
        case BiquadComboType::ButterworthHighpass:
        case BiquadComboType::ButterworthLowpass:
        case BiquadComboType::LinkwitzRileyHighpass:
        case BiquadComboType::LinkwitzRileyLowpass:
            f.comboParams.freq = stage.comboFreq;
            f.comboParams.order = stage.comboOrder;
            break;
        case BiquadComboType::Tilt:
            f.comboParams.freq = stage.comboFreq;
            f.comboParams.gain = stage.comboGain;
            break;
        case BiquadComboType::FivePointPeq:
            f.comboParams.fls = stage.peqFls;
            f.comboParams.gls = stage.peqGls;
            f.comboParams.qls = stage.peqQls;
            f.comboParams.fp1 = stage.peqF1;
            f.comboParams.gp1 = stage.peqG1;
            f.comboParams.qp1 = stage.peqQ1;
            f.comboParams.fp2 = stage.peqF2;
            f.comboParams.gp2 = stage.peqG2;
            f.comboParams.qp2 = stage.peqQ2;
            f.comboParams.fp3 = stage.peqF3;
            f.comboParams.gp3 = stage.peqG3;
            f.comboParams.qp3 = stage.peqQ3;
            f.comboParams.fhs = stage.peqFhs;
            f.comboParams.ghs = stage.peqGhs;
            f.comboParams.qhs = stage.peqQhs;
            break;
        default:
            break;
        }
        res.filters[prefix + "_combo"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_combo"}, std::nullopt});
        break;
    }

    case StageType::Limiter: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::Limiter;
        f.limiterParams.clipLimit = stage.limiterLimit;
        f.limiterParams.softClip = stage.limiterSoftClip;
        res.filters[prefix + "_limiter"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_limiter"}, std::nullopt});
        break;
    }

    case StageType::GraphicEQ: {
        if (chList.empty())
            break;
        FilterConfig f;
        f.type = FilterType::BiquadCombo;
        f.comboParams.type = BiquadComboType::GraphicEqualizer;
        f.comboParams.freqMin = stage.graphicEQFreqMin;
        f.comboParams.freqMax = stage.graphicEQFreqMax;
        f.comboParams.gains = stage.graphicEQGains;
        res.filters[prefix + "_geq"] = f;

        res.steps.push_back(PipelineStep{
            PipelineStepType::Filter, std::nullopt, chList, std::nullopt, {prefix + "_geq"}, std::nullopt});
        break;
    }
    }

    return res;
}
