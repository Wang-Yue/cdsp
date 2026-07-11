#ifndef SWEEP_DECONVOLVER_H
#define SWEEP_DECONVOLVER_H

#include "room_correction/ImpulseResponse.h"

#include <vector>

class SweepDeconvolver {
public:
    static ImpulseResponse deconvolve(const std::vector<double>& captured, double f1, double f2, double durationSeconds,
                                      int sampleRate);

    static std::vector<double> convolve(const std::vector<double>& a, const std::vector<double>& b);
};

#endif // SWEEP_DECONVOLVER_H
