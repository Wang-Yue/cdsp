#ifndef SWEEP_RECORDER_H
#define SWEEP_RECORDER_H

#include <optional>
#include <string>
#include <vector>

struct SweepCaptureResult {
    std::vector<double> captured;
    int roundTripSamples = 0;
    double peakAbsolute = 0.0;
};

class SweepRecorder {
public:
    static std::optional<int> locateSweepStart(const std::vector<double>& recording,
                                               const std::vector<double>& inverse);
    static std::vector<double> trimAndAlign(const std::vector<double>& captured, int startSample, size_t sweepLength,
                                            size_t tailSamples);
};

#endif // SWEEP_RECORDER_H
