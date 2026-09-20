#ifndef SWEEP_RECORDER_H
#define SWEEP_RECORDER_H

#include <optional> // for optional
#include <stddef.h> // for size_t
#include <string>   // for basic_string, string
#include <vector>   // for vector

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
    static SweepCaptureResult capture(double f1, double f2, double durationSeconds, int sampleRate,
                                      const std::string& inputDeviceName = "", const std::string& outputDeviceName = "",
                                      int inputChannel = 0, int outputChannel = -1, double playbackGainDB = -12.0);
};

#endif // SWEEP_RECORDER_H
