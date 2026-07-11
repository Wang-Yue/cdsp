#include "room_correction/SubwooferAssist.h"
#include "room_correction/SweepDeconvolver.h"
#include <cmath>
#include <algorithm>

SubwooferRecommendation SubwooferAssist::recommend(const ImpulseResponse& mainsIR, const ImpulseResponse& subIR) {
    SubwooferRecommendation rec;
    if (mainsIR.samples.empty() || subIR.samples.empty() || mainsIR.sampleRate <= 0) {
        return rec;
    }

    double fs = static_cast<double>(mainsIR.sampleRate);
    int searchRange = static_cast<int>(fs * 0.05); // +/- 50ms search window

    size_t mainsPeak = mainsIR.peakIndex();
    size_t subPeak = subIR.peakIndex();

    double maxCorr = -1e9;
    double minCorr = 1e9;
    int bestLag = 0;

    for (int lag = -searchRange; lag <= searchRange; ++lag) {
        double corrSum = 0.0;
        int count = 0;
        for (int i = 0; i < static_cast<int>(mainsIR.samples.size()); ++i) {
            int j = i + lag;
            if (j >= 0 && j < static_cast<int>(subIR.samples.size())) {
                corrSum += mainsIR.samples[i] * subIR.samples[j];
                count++;
            }
        }
        if (count > 0) {
            if (corrSum > maxCorr) {
                maxCorr = corrSum;
                bestLag = lag;
            }
            if (corrSum < minCorr) {
                minCorr = corrSum;
            }
        }
    }

    rec.delaySamples = bestLag;
    rec.delayMs = static_cast<double>(bestLag) / fs * 1000.0;

    // Direct cross-correlation polarity check (if inverted correlation peak magnitude is stronger)
    rec.invertSubPhase = (std::abs(minCorr) > maxCorr);
    rec.recommendedCrossoverHz = 80.0;

    return rec;
}
