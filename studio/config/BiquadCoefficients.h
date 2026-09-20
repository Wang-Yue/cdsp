#ifndef BIQUAD_COEFFICIENTS_H
#define BIQUAD_COEFFICIENTS_H

#include <QJsonObject> // for QJsonObject
#include <optional>    // for optional, operator==
#include <string>      // for string

enum class BiquadType {
    Free,
    Highpass,
    Lowpass,
    HighpassFO,
    LowpassFO,
    Highshelf,
    Lowshelf,
    HighshelfFO,
    LowshelfFO,
    Peaking,
    Notch,
    Bandpass,
    Allpass,
    AllpassFO,
    GeneralNotch,
    LinkwitzTransform
};

struct BiquadParameters {
    std::optional<BiquadType> type;
    std::optional<double> freq;
    std::optional<double> gain;
    std::optional<double> q;
    std::optional<double> bandwidth;
    std::optional<double> slope;

    // Free biquad
    std::optional<double> a1;
    std::optional<double> a2;
    std::optional<double> b0;
    std::optional<double> b1;
    std::optional<double> b2;

    // GeneralNotch
    std::optional<double> freqNotch;
    std::optional<double> freqPole;
    std::optional<double> qP;
    std::optional<bool> normalizeAtDc;

    // LinkwitzTransform
    std::optional<double> freqAct;
    std::optional<double> qAct;
    std::optional<double> freqTarget;
    std::optional<double> qTarget;

    QJsonObject toJson() const;
    static BiquadParameters fromJson(const QJsonObject& json);

    bool operator==(const BiquadParameters& other) const {
        return type == other.type && freq == other.freq && gain == other.gain && q == other.q &&
               bandwidth == other.bandwidth && slope == other.slope && a1 == other.a1 && a2 == other.a2 &&
               b0 == other.b0 && b1 == other.b1 && b2 == other.b2 && freqNotch == other.freqNotch &&
               freqPole == other.freqPole && qP == other.qP && normalizeAtDc == other.normalizeAtDc &&
               freqAct == other.freqAct && qAct == other.qAct && freqTarget == other.freqTarget &&
               qTarget == other.qTarget;
    }
    bool operator!=(const BiquadParameters& other) const { return !(*this == other); }
};

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    BiquadCoefficients() = default;
    BiquadCoefficients(double b0, double b1, double b2, double a1, double a2)
        : b0(b0), b1(b1), b2(b2), a1(a1), a2(a2) {}

    static const BiquadCoefficients passthrough;

    static std::optional<BiquadCoefficients> compute(const BiquadParameters& params, int sampleRate);

    double gainDB(double freqHz, int sampleRate) const;
    double phaseRad(double freqHz, int sampleRate) const;

    bool operator==(const BiquadCoefficients& other) const {
        return b0 == other.b0 && b1 == other.b1 && b2 == other.b2 && a1 == other.a1 && a2 == other.a2;
    }
    bool operator!=(const BiquadCoefficients& other) const { return !(*this == other); }
};

std::string biquadTypeToString(BiquadType type);
std::optional<BiquadType> stringToBiquadType(const std::string& str);

#endif // BIQUAD_COEFFICIENTS_H
