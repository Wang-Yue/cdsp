#ifndef IMPULSE_RESPONSE_H
#define IMPULSE_RESPONSE_H

#include <cstddef>
#include <vector>

struct RT60Result {
    double t20 = 0.0;
    double t30 = 0.0;
    double edt = 0.0;
};

class ImpulseResponse {
public:
    std::vector<double> samples;
    int sampleRate = 48000;
    size_t zeroIndex = 0;

    ImpulseResponse() = default;
    ImpulseResponse(const std::vector<double>& samples, int sampleRate = 48000, size_t zeroIndex = 0);

    size_t peakIndex() const;
    double peakValue() const;

    ImpulseResponse centeredOnPeak() const;
    ImpulseResponse windowed(size_t leftSamples, size_t rightSamples, double taperFraction = 0.1) const;
    std::vector<double> schroederDecay() const;
    RT60Result estimateRT60() const;
};

#endif // IMPULSE_RESPONSE_H
