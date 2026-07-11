#ifndef PIPELINE_STAGE_H
#define PIPELINE_STAGE_H

#include "config/DSPConfigTypes.h"
#include "models/EQPreset.h"
#include "models/ConvolutionPreset.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>

enum class StageCategory {
    Volume, EQ, Dynamics, Delay, Matrix
};

enum class StageType {
    Balance, Width, MSProc, PhaseInvert, Crossfeed, SplitWidth,
    EQ, GraphicEQ, Convolution, Loudness, Emphasis, DCProtection,
    Gain, Delay, LookaheadLimiter, Limiter, Volume, MatrixMixer,
    Compressor, NoiseGate, RACE, Dither, DiffEq, BiquadCombo
};

std::string stageCategoryToString(StageCategory cat);
std::string stageTypeToString(StageType type);
StageCategory stageTypeToCategory(StageType type);
std::string stageTypeToIcon(StageType type);

enum class CrossfeedPreset { Bauer, Meier, Moy, Custom };

class PipelineStage {
public:
    QUuid id;
    std::string name;
    StageType type;
    bool isEnabled = true;

    // Stage-specific parameters
    double balanceOffset = 0.0; // [-1.0, 1.0]
    double widthFactor = 1.0;   // [0.0, 2.0]
    bool invertLeft = false;
    bool invertRight = false;

    // Crossfeed
    CrossfeedPreset crossfeedPreset = CrossfeedPreset::Bauer;
    double crossfeedCutoff = 700.0;
    double crossfeedFeedDB = -4.5;

    // Split Width
    double splitFreq = 1000.0;
    double lowWidth = 0.0;
    double highWidth = 1.0;

    // EQ reference preset ID
    std::optional<QUuid> eqPresetId;

    // Graphic EQ gains
    std::vector<double> graphicEqGains;

    // Convolution reference preset ID
    std::optional<QUuid> convPresetId;

    // Loudness
    double loudnessRefLevel = 83.0;
    double loudnessHighBoost = 0.0;
    double loudnessLowBoost = 0.0;
    bool loudnessAttenuateMid = false;

    // Emphasis
    bool deEmphasis = false;

    // DC Protection
    double dcCutoffFreq = 10.0;

    // Gain
    double gainDB = 0.0;
    bool gainInverted = false;
    bool gainMuted = false;

    // Delay
    double delayValue = 0.0;
    DelayUnit delayUnit = DelayUnit::ms;

    // Limiter
    double limiterThreshold = 0.0;
    bool limiterSoftClip = true;

    // Lookahead Limiter
    double lookaheadLimit = 0.0;
    double lookaheadAttack = 5.0;
    double lookaheadRelease = 100.0;

    // Matrix Mixer config
    MixerConfig mixerConfig;

    // Compressor
    CompressorParameters compressorParams;

    // Noise Gate
    NoiseGateParameters noiseGateParams;

    // RACE
    RACEParameters raceParams;

    // Dither
    DitherType ditherType = DitherType::Flat;
    int ditherBits = 16;

    // DiffEq
    std::vector<double> diffEqA;
    std::vector<double> diffEqB;

    // BiquadCombo
    BiquadComboParameters comboParams;

    PipelineStage();
    explicit PipelineStage(StageType type, const std::string& name = "");

    QJsonObject toJson() const;
    static PipelineStage fromJson(const QJsonObject& json);

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
    static StageBuildResult buildStage(
        const PipelineStage& stage,
        int sampleRate,
        int channelCount,
        const std::map<QUuid, EQPreset>& eqPresets,
        const std::map<QUuid, ConvolutionPreset>& convPresets
    );
};

#endif // PIPELINE_STAGE_H
