#include "room_correction/SweepRecorder.h"

#include "room_correction/SweepDeconvolver.h"

#include <algorithm>
#include <cmath>

std::optional<int> SweepRecorder::locateSweepStart(const std::vector<double>& recording,
                                                   const std::vector<double>& inverse) {
    if (recording.empty() || inverse.empty())
        return std::nullopt;

    std::vector<double> convolved = SweepDeconvolver::convolve(recording, inverse);
    size_t peakIdx = 0;
    double peakAbs = 0.0;

    for (size_t i = 0; i < convolved.size(); ++i) {
        double v = std::abs(convolved[i]);
        if (v > peakAbs) {
            peakAbs = v;
            peakIdx = i;
        }
    }

    if (peakAbs <= 0.0)
        return std::nullopt;
    int startSample = static_cast<int>(peakIdx) - static_cast<int>(inverse.size() - 1);
    return startSample;
}

std::vector<double> SweepRecorder::trimAndAlign(const std::vector<double>& captured, int startSample,
                                                size_t sweepLength, size_t tailSamples) {
    size_t needed = sweepLength + tailSamples;
    std::vector<double> out(needed, 0.0);

    for (size_t i = 0; i < needed; ++i) {
        int srcIdx = startSample + static_cast<int>(i);
        if (srcIdx >= 0 && static_cast<size_t>(srcIdx) < captured.size()) {
            out[i] = captured[srcIdx];
        }
    }
    return out;
}
