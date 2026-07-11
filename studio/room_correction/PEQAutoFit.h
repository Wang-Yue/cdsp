#ifndef PEQ_AUTO_FIT_H
#define PEQ_AUTO_FIT_H

#include "config/BiquadCoefficients.h"
#include "room_correction/TargetCurve.h"
#include "room_correction/FrequencyResponse.h"
#include <vector>

struct PEQAutoFitOptions {
    int bandCount = 8;
    double maxGainDB = 12.0;
    bool modalMode = false;
    double schroederHz = 200.0;
    double modalMinQ = 2.0;
};

class PEQAutoFit {
public:
    static std::vector<double> logFrequencyGrid(double fMin = 20.0, double fMax = 20000.0, size_t count = 256);

    static std::vector<double> smoothLogOctave(const std::vector<double>& magDB, const std::vector<double>& frequencies, double octaves = 1.0 / 6.0);

    static std::vector<double> sampleMagnitudeDB(const FrequencyResponse& fr, const std::vector<double>& atFrequencies);

    static std::vector<BiquadParameters> fit(
        const std::vector<double>& measuredMagDB,
        const std::vector<double>& frequencies,
        const TargetCurve& target,
        int sampleRate,
        const PEQAutoFitOptions& options = PEQAutoFitOptions()
    );
};

#endif // PEQ_AUTO_FIT_H
