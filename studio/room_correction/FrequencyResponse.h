#ifndef FREQUENCY_RESPONSE_H
#define FREQUENCY_RESPONSE_H

#include "room_correction/ImpulseResponse.h"

#include <tuple>
#include <vector>

class FrequencyResponse {
public:
    std::vector<double> real;
    std::vector<double> imag;
    int sampleRate = 48000;
    int fftSize = 4096;

    FrequencyResponse() = default;
    FrequencyResponse(const std::vector<double>& real, const std::vector<double>& imag, int sampleRate = 48000,
                      int fftSize = 4096);

    size_t bins() const { return real.size(); }
    double frequency(size_t bin) const;

    double magnitude(size_t bin) const;
    double magnitudeDB(size_t bin) const;
    double phase(size_t bin) const;

    std::vector<double> unwrappedPhase() const;
    std::vector<double> groupDelay() const;

    static FrequencyResponse from(const ImpulseResponse& ir, int fftSize = 4096);
    static FrequencyResponse fdw(const ImpulseResponse& ir, double cycles, int fftSize = 4096);

    static std::vector<std::pair<double, FrequencyResponse>> stft(const ImpulseResponse& ir, int sliceCount = 30,
                                                                  double maxTimeSeconds = 0.4, int windowLength = 2048,
                                                                  int fftSize = 4096);
};

#endif // FREQUENCY_RESPONSE_H
