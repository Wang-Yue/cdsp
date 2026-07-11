#include "room_correction/SubwooferAssist.h"
#include "room_correction/SweepDeconvolver.h"
#include <cmath>

SubwooferRecommendation SubwooferAssist::recommend(const ImpulseResponse& mainsIR, const ImpulseResponse& subIR) {
    SubwooferRecommendation rec;

    size_t mainsPeak = mainsIR.peakIndex();
    size_t subPeak = subIR.peakIndex();

    int diffSamples = static_cast<int>(subPeak) - static_cast<int>(mainsPeak);
    double fs = static_cast<double>(mainsIR.sampleRate);

    rec.delaySamples = diffSamples;
    rec.delayMs = static_cast<double>(diffSamples) / fs * 1000.0;

    // Phase inversion test
    rec.invertSubPhase = (subIR.samples[subPeak] * mainsIR.samples[mainsPeak] < 0);
    rec.recommendedCrossoverHz = 80.0;

    return rec;
}
