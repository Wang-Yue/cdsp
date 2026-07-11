#ifndef PEQ_AUTO_FIT_H
#define PEQ_AUTO_FIT_H

#include "config/BiquadCoefficients.h"
#include "room_correction/FrequencyResponse.h"
#include "room_correction/TargetCurve.h"

#include <vector>

struct PEQAutoFitOptions {
    int bandCount = 10;
    double minFreqHz = 20.0;
    double maxFreqHz = 20000.0;
    double maxGainDB = 12.0;
    double minQ = 0.3;
    double maxQ = 10.0;
    double convergenceDB = 0.3;
    bool addEndpointShelves = true;
    double lowShelfFreqHz = 80.0;
    double highShelfFreqHz = 8000.0;
    int refinementIterations = 8;
    double dropGainDB = 0.5;
    bool modalMode = false;
    double schroederHz = 200.0;
    double modalMinQ = 2.0;
    double smoothingOctaves = 1.0 / 12.0;
    double trebleSmoothingOctaves = 2.0;
    double smoothingTransitionLow = 6000.0;
    double smoothingTransitionHigh = 8000.0;
};

class PEQAutoFit {
public:
    static std::vector<double> logFrequencyGrid(double fMin = 20.0, double fMax = 20000.0, size_t count = 256);

    static std::vector<double> smoothLogOctave(const std::vector<double>& magDB, const std::vector<double>& frequencies,
                                               double octaves = 1.0 / 12.0);

    static std::vector<double> smoothLogOctave(const std::vector<double>& magDB, const std::vector<double>& frequencies,
                                               double midOctaves, double trebleOctaves, double transitionLowHz,
                                               double transitionHighHz);

    static std::vector<double> sampleMagnitudeDB(const FrequencyResponse& fr, const std::vector<double>& atFrequencies);

    static std::vector<BiquadParameters> fit(const std::vector<double>& measuredMagDB,
                                             const std::vector<double>& frequencies, const TargetCurve& target,
                                             int sampleRate, const PEQAutoFitOptions& options = PEQAutoFitOptions());
};

#endif // PEQ_AUTO_FIT_H
