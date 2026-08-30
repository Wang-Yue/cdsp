#ifndef PIPELINE_STAGE_H
#define PIPELINE_STAGE_H

#include "config/DSPConfigTypes.h"    // for TimeUnit, MixerMapping, DelayUnit, Fader, PipelineStep, BiquadComboType
#include "models/ConvolutionPreset.h" // for ConvolutionPreset
#include "models/EQPreset.h"          // for EQPreset

#include <QJsonObject> // for QJsonObject
#include <QUuid>       // for QUuid
#include <map>         // for map
#include <optional>    // for optional
#include <string>      // for basic_string, string
#include <vector>      // for vector

enum class StageCategory { Filters, Mixer, Processors, Others };

enum class StageType {
    Balance,
    Width,
    MSProc,
    PhaseInvert,
    Crossfeed,
    SplitWidth,
    EQ,
    GraphicEQ,
    Convolution,
    Loudness,
    Emphasis,
    DCProtection,
    Gain,
    Delay,
    LookaheadLimiter,
    LookaheadLimiterProc,
    Clipper,
    Volume,
    MatrixMixer,
    Compressor,
    NoiseGate,
    RACE,
    Dither,
    DiffEq,
    BiquadCombo
};

std::string stageCategoryToString(StageCategory cat);
std::string stageTypeToString(StageType type);
StageType stringToStageType(const std::string& str);
StageCategory stageTypeToCategory(StageType type);

enum class CrossfeedLevel { Off, L1, L2, L3, L4, L5 };
std::string crossfeedLevelToString(CrossfeedLevel l);
CrossfeedLevel stringToCrossfeedLevel(const std::string& str);
std::string crossfeedLevelDescription(CrossfeedLevel l);

enum class EmphasisMode { Off, DeEmphasis, PreEmphasis };
std::string emphasisModeToString(EmphasisMode m);
EmphasisMode stringToEmphasisMode(const std::string& str);
std::string emphasisModeDescription(EmphasisMode m);

struct CrossfeedParamsResult {
    double hiFreq;
    double hiGain;
    double hiQ;
    double loFreq;
    double loGain;
};

class PipelineStage {
public:
    QUuid id;
    StageType type;
    std::string name;
    bool isEnabled = false;

    // Dynamic channel mapping
    std::vector<int> channels = {0, 1};
    std::vector<int> monitorChannels = {0, 1};

    // Stereo-specific channel routing
    int leftChannel = 0;
    int rightChannel = 1;

    // Stage-specific parameters
    double balancePosition = 0.0;
    double widthAmount = 1.0;
    CrossfeedLevel crossfeedLevel = CrossfeedLevel::L1;
    bool cxCustomEnabled = false;
    double cxFc = 650.0;
    double cxDb = 13.5;

    std::optional<QUuid> eqPresetId;
    std::optional<QUuid> convPresetId;
    EmphasisMode emphasisMode = EmphasisMode::DeEmphasis;
    double loudnessReference = -25.0;
    double loudnessHighBoost = 7.0;
    double loudnessLowBoost = 7.0;
    Fader loudnessFader = Fader::Main;
    bool loudnessAttenuateMid = false;

    double gainValue = 0.0;
    bool gainInverted = false;
    bool gainMuted = false;

    double volumeRampTime = 400.0;
    double volumeLimit = 10.0;
    Fader volumeFader = Fader::Aux1;

    double delayValue = 0.0;
    DelayUnit delayUnit = DelayUnit::ms;
    bool delaySubsample = false;

    double lookaheadLimit = 0.0;
    double lookaheadAttack = 5.0;
    TimeUnit lookaheadAttackUnit = TimeUnit::ms;
    double lookaheadRelease = 100.0;
    TimeUnit lookaheadReleaseUnit = TimeUnit::ms;
    bool lookaheadDelayProcessedOnly = false;

    int mixerChannelsIn = 2;
    int mixerChannelsOut = 2;
    std::vector<MixerMapping> mixerMappings;

    double compressorAttack = 5.0;
    TimeUnit compressorAttackUnit = TimeUnit::ms;
    double compressorRelease = 100.0;
    TimeUnit compressorReleaseUnit = TimeUnit::ms;
    double compressorThreshold = -20.0;
    double compressorRatio = 2.0;
    double compressorMakeupGain = 0.0;
    bool compressorSoftClip = false;
    double compressorClipLimit = 0.0;

    double gateAttack = 5.0;
    TimeUnit gateAttackUnit = TimeUnit::ms;
    double gateRelease = 100.0;
    TimeUnit gateReleaseUnit = TimeUnit::ms;
    double gateThreshold = -60.0;
    double gateAttenuation = -40.0;

    double raceDelay = 0.25;
    double raceAttenuation = 6.0;
    bool raceSubsampleDelay = false;
    DelayUnit raceDelayUnit = DelayUnit::ms;

    DitherType ditherType = DitherType::Flat;
    int ditherBits = 16;
    double ditherAmplitude = 1.0;

    std::string diffEqA = "1.0, 0.5";
    std::string diffEqB = "0.5, 0.25";

    BiquadComboType comboType = BiquadComboType::ButterworthLowpass;
    double comboFreq = 1000.0;
    int comboOrder = 2;
    double comboGain = 0.0;
    std::string comboGains = "0.0, 0.0, 0.0, 0.0, 0.0";
    double comboFreqMin = 20.0;
    double comboFreqMax = 20000.0;

    double peqFls = 80.0;
    double peqGls = 0.0;
    double peqQls = 0.707;
    double peqF1 = 200.0;
    double peqG1 = 0.0;
    double peqQ1 = 0.707;
    double peqF2 = 1000.0;
    double peqG2 = 0.0;
    double peqQ2 = 0.707;
    double peqF3 = 4000.0;
    double peqG3 = 0.0;
    double peqQ3 = 0.707;
    double peqFhs = 12000.0;
    double peqGhs = 0.0;
    double peqQhs = 0.707;

    double clipperLimit = 0.0;
    bool clipperSoftClip = false;

    double splitWidthCrossover = 150.0;
    double splitWidthAmount = 1.5;

    double graphicEQFreqMin = 20.0;
    double graphicEQFreqMax = 20000.0;
    int graphicEQBandCount = 31;
    std::vector<double> graphicEQGains = std::vector<double>(31, 0.0);

    PipelineStage();
    explicit PipelineStage(StageType type, const std::string& name = "", bool isEnabled = false,
                           const std::vector<int>& channels = {0, 1});

    void setGraphicEQBandCount(int count);
    double getGraphicEQGain(int index) const;
    void setGraphicEQGain(int index, double gain);
    double getMixerSourceGain(int mappingIndex, int sourceIndex) const;
    void setMixerSourceGain(int mappingIndex, int sourceIndex, double gain);

    int balanceLeftPercent() const;
    int balanceRightPercent() const;
    int widthPercent() const;
    std::string widthDescription() const;
    bool isActive() const;

    static CrossfeedParamsResult computeCrossfeed(double fc, double db);
    CrossfeedParamsResult activeCrossfeedParams() const;

    QJsonObject toJson() const;
    static PipelineStage fromJson(const QJsonObject& json);
    static std::vector<PipelineStage> defaultStages();

    bool operator==(const PipelineStage& other) const;
};

struct StageBuildResult {
    std::map<std::string, FilterConfig> filters;
    std::map<std::string, MixerConfig> mixers;
    std::map<std::string, ProcessorConfig> processors;
    std::vector<PipelineStep> steps;
};

class StageBuilders {
public:
    static StageBuildResult buildStage(const PipelineStage& stage, int sampleRate, int channelCount,
                                       const std::map<QUuid, EQPreset>& eqPresets,
                                       const std::map<QUuid, ConvolutionPreset>& convPresets);
};

#endif // PIPELINE_STAGE_H
