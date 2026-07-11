#include "config/BiquadCoefficients.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const BiquadCoefficients BiquadCoefficients::passthrough(1.0, 0.0, 0.0, 0.0, 0.0);

std::string biquadTypeToString(BiquadType type) {
    switch (type) {
    case BiquadType::Free:
        return "Free";
    case BiquadType::Highpass:
        return "Highpass";
    case BiquadType::Lowpass:
        return "Lowpass";
    case BiquadType::HighpassFO:
        return "HighpassFO";
    case BiquadType::LowpassFO:
        return "LowpassFO";
    case BiquadType::Highshelf:
        return "Highshelf";
    case BiquadType::Lowshelf:
        return "Lowshelf";
    case BiquadType::HighshelfFO:
        return "HighshelfFO";
    case BiquadType::LowshelfFO:
        return "LowshelfFO";
    case BiquadType::Peaking:
        return "Peaking";
    case BiquadType::Notch:
        return "Notch";
    case BiquadType::Bandpass:
        return "Bandpass";
    case BiquadType::Allpass:
        return "Allpass";
    case BiquadType::AllpassFO:
        return "AllpassFO";
    case BiquadType::GeneralNotch:
        return "GeneralNotch";
    case BiquadType::LinkwitzTransform:
        return "LinkwitzTransform";
    }
    return "Peaking";
}

std::optional<BiquadType> stringToBiquadType(const std::string& str) {
    if (str == "Free")
        return BiquadType::Free;
    if (str == "Highpass")
        return BiquadType::Highpass;
    if (str == "Lowpass")
        return BiquadType::Lowpass;
    if (str == "HighpassFO")
        return BiquadType::HighpassFO;
    if (str == "LowpassFO")
        return BiquadType::LowpassFO;
    if (str == "Highshelf")
        return BiquadType::Highshelf;
    if (str == "Lowshelf")
        return BiquadType::Lowshelf;
    if (str == "HighshelfFO")
        return BiquadType::HighshelfFO;
    if (str == "LowshelfFO")
        return BiquadType::LowshelfFO;
    if (str == "Peaking")
        return BiquadType::Peaking;
    if (str == "Notch")
        return BiquadType::Notch;
    if (str == "Bandpass")
        return BiquadType::Bandpass;
    if (str == "Allpass")
        return BiquadType::Allpass;
    if (str == "AllpassFO")
        return BiquadType::AllpassFO;
    if (str == "GeneralNotch")
        return BiquadType::GeneralNotch;
    if (str == "LinkwitzTransform")
        return BiquadType::LinkwitzTransform;
    return std::nullopt;
}

std::optional<BiquadCoefficients> BiquadCoefficients::compute(const BiquadParameters& params, int sampleRate) {
    if (!params.type.has_value())
        return std::nullopt;

    BiquadType type = params.type.value();
    double fs = static_cast<double>(sampleRate);
    double freq = params.freq.value_or(1000.0);
    double gain = params.gain.value_or(0.0);
    double q = params.q.value_or(0.707);

    double w0 = 0.0;
    double cosW0 = 0.0;
    double sinW0 = 0.0;
    double A = 1.0;
    double alpha = 0.0;

    bool needsW0 =
        (type != BiquadType::Free && type != BiquadType::GeneralNotch && type != BiquadType::LinkwitzTransform);

    if (needsW0) {
        w0 = 2.0 * M_PI * freq / fs;
        cosW0 = std::cos(w0);
        sinW0 = std::sin(w0);
        A = std::pow(10.0, gain / 40.0);

        if (params.bandwidth.has_value()) {
            double bw = params.bandwidth.value();
            q = 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * bw * w0 / sinW0));
        } else if (params.slope.has_value()) {
            double s = params.slope.value();
            double slopeS = s / 12.0;
            q = 1.0 / std::sqrt((A + 1.0 / A) * (1.0 / slopeS - 1.0) + 2.0);
        }

        alpha = sinW0 / (2.0 * q);
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
    case BiquadType::Free:
        b0 = params.b0.value_or(1.0);
        b1 = params.b1.value_or(0.0);
        b2 = params.b2.value_or(0.0);
        a0 = 1.0;
        a1 = params.a1.value_or(0.0);
        a2 = params.a2.value_or(0.0);
        break;

    case BiquadType::GeneralNotch: {
        double freqZ = params.freqNotch.value_or(1000.0);
        double freqP = params.freqPole.value_or(1000.0);
        double qP = params.qP.value_or(params.q.value_or(0.5));
        bool normalize = params.normalizeAtDc.value_or(false);
        double tnZ = std::tan(M_PI * freqZ / fs);
        double tnP = std::tan(M_PI * freqP / fs);
        double alphaP = tnP / qP;
        double tn2P = tnP * tnP;
        double tn2Z = tnZ * tnZ;
        double gainNorm = normalize ? tn2P / tn2Z : 1.0;
        b0 = gainNorm * (1.0 + tn2Z);
        b1 = -2.0 * gainNorm * (1.0 - tn2Z);
        b2 = gainNorm * (1.0 + tn2Z);
        a0 = 1.0 + alphaP + tn2P;
        a1 = -2.0 + 2.0 * tn2P;
        a2 = 1.0 - alphaP + tn2P;
        break;
    }

    case BiquadType::LinkwitzTransform: {
        double freqAct = params.freqAct.value_or(50.0);
        double qAct = params.qAct.value_or(0.707);
        double freqTarget = params.freqTarget.value_or(25.0);
        double qTarget = params.qTarget.value_or(0.707);
        double d0i = std::pow(2.0 * M_PI * freqAct, 2);
        double d1i = (2.0 * M_PI * freqAct) / qAct;
        double c0i = std::pow(2.0 * M_PI * freqTarget, 2);
        double c1i = (2.0 * M_PI * freqTarget) / qTarget;
        double fc = (freqTarget + freqAct) / 2.0;
        double gn = 2.0 * M_PI * fc / std::tan(M_PI * fc / fs);
        double gn2 = gn * gn;
        double cci = c0i + gn * c1i + gn2;
        b0 = (d0i + gn * d1i + gn2) / cci;
        b1 = 2.0 * (d0i - gn2) / cci;
        b2 = (d0i - gn * d1i + gn2) / cci;
        a0 = 1.0;
        a1 = 2.0 * (c0i - gn2) / cci;
        a2 = (c0i - gn * c1i + gn2) / cci;
        break;
    }

    case BiquadType::Peaking:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cosW0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha / A;
        break;

    case BiquadType::Lowshelf:
        b0 = A * ((A + 1.0) - (A - 1.0) * cosW0 + 2.0 * std::sqrt(A) * alpha);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0);
        b2 = A * ((A + 1.0) - (A - 1.0) * cosW0 - 2.0 * std::sqrt(A) * alpha);
        a0 = (A + 1.0) + (A - 1.0) * cosW0 + 2.0 * std::sqrt(A) * alpha;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW0);
        a2 = (A + 1.0) + (A - 1.0) * cosW0 - 2.0 * std::sqrt(A) * alpha;
        break;

    case BiquadType::Highshelf:
        b0 = A * ((A + 1.0) + (A - 1.0) * cosW0 + 2.0 * std::sqrt(A) * alpha);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0);
        b2 = A * ((A + 1.0) + (A - 1.0) * cosW0 - 2.0 * std::sqrt(A) * alpha);
        a0 = (A + 1.0) - (A - 1.0) * cosW0 + 2.0 * std::sqrt(A) * alpha;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW0);
        a2 = (A + 1.0) - (A - 1.0) * cosW0 - 2.0 * std::sqrt(A) * alpha;
        break;

    case BiquadType::Lowpass:
        b0 = (1.0 - cosW0) / 2.0;
        b1 = 1.0 - cosW0;
        b2 = (1.0 - cosW0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case BiquadType::Highpass:
        b0 = (1.0 + cosW0) / 2.0;
        b1 = -(1.0 + cosW0);
        b2 = (1.0 + cosW0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case BiquadType::Notch:
        b0 = 1.0;
        b1 = -2.0 * cosW0;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case BiquadType::Bandpass:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case BiquadType::Allpass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cosW0;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case BiquadType::LowpassFO:
        b0 = sinW0;
        b1 = sinW0;
        b2 = 0.0;
        a0 = sinW0 + 1.0 + cosW0;
        a1 = sinW0 - 1.0 - cosW0;
        a2 = 0.0;
        break;

    case BiquadType::HighpassFO:
        b0 = 1.0 + cosW0;
        b1 = -1.0 - cosW0;
        b2 = 0.0;
        a0 = sinW0 + 1.0 + cosW0;
        a1 = sinW0 - 1.0 - cosW0;
        a2 = 0.0;
        break;

    case BiquadType::LowshelfFO:
        b0 = A * sinW0 + 1.0 + cosW0;
        b1 = A * sinW0 - 1.0 - cosW0;
        b2 = 0.0;
        a0 = (1.0 / A) * sinW0 + 1.0 + cosW0;
        a1 = (1.0 / A) * sinW0 - 1.0 - cosW0;
        a2 = 0.0;
        break;

    case BiquadType::HighshelfFO:
        b0 = sinW0 + A + A * cosW0;
        b1 = sinW0 - A - A * cosW0;
        b2 = 0.0;
        a0 = sinW0 + (1.0 / A) + (1.0 / A) * cosW0;
        a1 = sinW0 - (1.0 / A) - (1.0 / A) * cosW0;
        a2 = 0.0;
        break;

    case BiquadType::AllpassFO:
        b0 = sinW0 - 1.0 - cosW0;
        b1 = sinW0 + 1.0 + cosW0;
        b2 = 0.0;
        a0 = sinW0 + 1.0 + cosW0;
        a1 = sinW0 - 1.0 - cosW0;
        a2 = 0.0;
        break;
    }

    return BiquadCoefficients(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
}

double BiquadCoefficients::gainDB(double f, int sampleRate) const {
    double w = 2.0 * M_PI * f / static_cast<double>(sampleRate);
    double cosW = std::cos(w);
    double sinW = std::sin(w);
    double cos2W = std::cos(2.0 * w);
    double sin2W = std::sin(2.0 * w);

    double numRe = b0 + b1 * cosW + b2 * cos2W;
    double numIm = -b1 * sinW - b2 * sin2W;
    double denRe = 1.0 + a1 * cosW + a2 * cos2W;
    double denIm = -a1 * sinW - a2 * sin2W;

    double numMagSq = numRe * numRe + numIm * numIm;
    double denMagSq = denRe * denRe + denIm * denIm;

    if (denMagSq <= 0.0)
        return 0.0;
    double ratio = std::max(1e-12, numMagSq / denMagSq);
    return 10.0 * std::log10(ratio);
}

double BiquadCoefficients::phaseRad(double f, int sampleRate) const {
    double w = 2.0 * M_PI * f / static_cast<double>(sampleRate);
    double cosW = std::cos(w);
    double sinW = std::sin(w);
    double cos2W = std::cos(2.0 * w);
    double sin2W = std::sin(2.0 * w);

    double numRe = b0 + b1 * cosW + b2 * cos2W;
    double numIm = -b1 * sinW - b2 * sin2W;
    double denRe = 1.0 + a1 * cosW + a2 * cos2W;
    double denIm = -a1 * sinW - a2 * sin2W;

    double denMagSq = denRe * denRe + denIm * denIm;
    if (denMagSq <= 0.0)
        return 0.0;

    double hRe = (numRe * denRe + numIm * denIm) / denMagSq;
    double hIm = (numIm * denRe - numRe * denIm) / denMagSq;

    return std::atan2(hIm, hRe);
}
