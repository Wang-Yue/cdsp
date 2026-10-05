#ifndef SWEEP_RECORDER_H
#define SWEEP_RECORDER_H

#include <functional> // for function
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
    /**
     * Plays the sweep and records it without blocking. Must be called on a thread running a Qt event loop (the GUI
     * thread): Qt audio on WebAssembly works only there, and nested event loops are unavailable on its main thread.
     * @p done is invoked on a worker thread once the recording has been aligned.
     */
    static void capture(double f1, double f2, double durationSeconds, int sampleRate,
                        const std::string& inputDeviceName, const std::string& outputDeviceName, int inputChannel,
                        int outputChannel, double playbackGainDB, std::function<void(SweepCaptureResult)> done);
};

#endif // SWEEP_RECORDER_H
