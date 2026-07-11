#ifndef EQ_PRESET_H
#define EQ_PRESET_H

#include "config/BiquadCoefficients.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>

enum class EQBandType {
    Peaking, Lowshelf, Highshelf, Lowpass, Highpass,
    LowpassFO, HighpassFO, LowshelfFO, HighshelfFO,
    Notch, Bandpass, Allpass, AllpassFO,
    Free, GeneralNotch, LinkwitzTransform
};

std::string eqBandTypeToString(EQBandType type);
EQBandType stringToEQBandType(const std::string& str);
std::string eqBandTypeToShortName(EQBandType type);
EQBandType shortNameToEQBandType(const std::string& str);

bool eqBandTypeHasGain(EQBandType type);
bool eqBandTypeHasQ(EQBandType type);

class EQBand {
public:
    QUuid id;
    EQBandType type = EQBandType::Peaking;
    double freq = 1000.0;
    double gain = 0.0;
    double q = 0.707;
    bool isEnabled = true;

    // Free biquad coefficients
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    // GeneralNotch
    double freqNotch = 1000.0;
    double freqPole = 1000.0;
    double qPole = 0.707;
    bool normalizeAtDc = true;

    // Slope / Bandwidth
    double slope = 6.0;
    double bandwidth = 1.0;
    bool useSlope = false;
    bool useBandwidth = false;

    // Linkwitz Transform
    double freqAct = 50.0;
    double qAct = 0.707;
    double freqTarget = 20.0;
    double qTarget = 0.707;

    EQBand();
    EQBand(EQBandType type, double freq = 1000.0, double gain = 0.0, double q = 0.707, bool enabled = true);

    std::optional<BiquadCoefficients> coefficients(int sampleRate) const;
    double response(double f, int sampleRate) const;
    double phaseResponse(double f, int sampleRate) const;

    QJsonObject toJson() const;
    static EQBand fromJson(const QJsonObject& json);

    bool operator==(const EQBand& other) const;
};

class EQPreset {
public:
    QUuid id;
    std::string name;
    double preampGain = -6.0;
    std::vector<EQBand> bands;

    EQPreset();
    EQPreset(const std::string& name, double preampGain = -6.0, const std::vector<EQBand>& bands = {});

    void addBand(const EQBand& band = EQBand());
    void removeBand(size_t index);

    double combinedResponse(double f, int sampleRate) const;
    double combinedPhase(double f, int sampleRate) const;

    std::string toCSV() const;
    static std::optional<EQPreset> fromCSV(const std::string& text, const std::string& presetName = "Imported Preset");

    QJsonObject toJson() const;
    static EQPreset fromJson(const QJsonObject& json);

    bool operator==(const EQPreset& other) const;
};

#endif // EQ_PRESET_H
