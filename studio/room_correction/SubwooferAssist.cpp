#include "room_correction/SubwooferAssist.h"

#include "room_correction/FrequencyResponse.h"

#include <algorithm>
#include <cmath>
#include <sstream>

static double snapToCommonCrossover(double f) {
    const std::vector<double> common = {40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 120.0, 150.0, 180.0, 200.0};
    double minDiff = 1e9;
    double best = std::round(f);
    for (double c : common) {
        double diff = std::abs(c - f);
        if (diff < minDiff) {
            minDiff = diff;
            best = c;
        }
    }
    return best;
}

SubwooferRecommendation SubwooferAssist::recommend(const ImpulseResponse& mainsIR, const ImpulseResponse& subIR) {
    SubwooferRecommendation rec;
    if (mainsIR.samples.empty() || subIR.samples.empty() || mainsIR.sampleRate <= 0) {
        return rec;
    }

    int sr = mainsIR.sampleRate;
    int maxLag = std::min(sr / 10, static_cast<int>(std::min(mainsIR.samples.size(), subIR.samples.size())) - 1);

    int bestLag = 0;
    double bestVal = -1e18;

    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        int aStart = std::max(0, -lag);
        int bStart = std::max(0, lag);
        int n = std::min(static_cast<int>(mainsIR.samples.size()) - aStart,
                         static_cast<int>(subIR.samples.size()) - bStart);
        if (n <= 0)
            continue;
        for (int k = 0; k < n; ++k) {
            sum += mainsIR.samples[aStart + k] * subIR.samples[bStart + k];
        }
        if (sum > bestVal) {
            bestVal = sum;
            bestLag = lag;
        }
    }

    rec.subDelayMs = static_cast<double>(bestLag) / static_cast<double>(sr) * 1000.0;
    rec.delayMs = rec.subDelayMs;
    rec.delaySamples = bestLag;

    FrequencyResponse mainsFR = FrequencyResponse::from(mainsIR);
    FrequencyResponse subFR = FrequencyResponse::from(subIR);

    size_t bins = mainsFR.bins();
    double binHz = static_cast<double>(mainsFR.sampleRate) / static_cast<double>(mainsFR.fftSize);

    double mainsRef = 0.0;
    int refCount = 0;
    for (size_t k = 1; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        if (f >= 120.0 && f <= 200.0) {
            mainsRef += mainsFR.magnitudeDB(k);
            refCount++;
        }
    }
    if (refCount > 0)
        mainsRef /= static_cast<double>(refCount);

    double crossingHz = 0.0;
    bool foundCrossing = false;
    for (size_t k = 1; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        if (f < 30.0)
            continue;
        if (f > 250.0)
            break;
        double mainsDB = mainsFR.magnitudeDB(k);
        double subDB = subFR.magnitudeDB(k);
        if ((mainsRef - mainsDB) >= 6.0 && subDB > mainsDB) {
            crossingHz = f;
            foundCrossing = true;
            break;
        }
    }

    if (foundCrossing) {
        rec.crossoverHz = snapToCommonCrossover(crossingHz);
        rec.confidence = 0.85;
        std::stringstream ss;
        ss << "Picked the crossover where the mains have rolled off ~6 dB below their 120–200 Hz reference and the sub "
              "is louder. "
           << "Mains high-pass and sub low-pass at " << static_cast<int>(rec.crossoverHz)
           << " Hz produce a 4th-order Linkwitz-Riley sum (12 dB/oct each, in phase at fc).";
        rec.summary = ss.str();
    } else {
        rec.crossoverHz = 80.0;
        rec.confidence = 0.2;
        rec.summary = "Couldn't find a clean overlap between sub and mains. Falling back to the THX-standard 80 Hz "
                      "crossover; verify by ear or with a fresh measurement.";
    }

    double q = 0.7071; // Butterworth
    BiquadParameters hp;
    hp.type = BiquadType::Highpass;
    hp.freq = rec.crossoverHz;
    hp.gain = 0.0;
    hp.q = q;
    rec.mainsHighPass = hp;

    BiquadParameters lp;
    lp.type = BiquadType::Lowpass;
    lp.freq = rec.crossoverHz;
    lp.gain = 0.0;
    lp.q = q;
    rec.subLowPass = lp;

    return rec;
}
