#ifndef SUBWOOFER_ASSIST_H
#define SUBWOOFER_ASSIST_H

#include "room_correction/ImpulseResponse.h"
#include "config/BiquadCoefficients.h"
#include <string>

struct SubwooferRecommendation {
    double subDelayMs = 0.0;
    double crossoverHz = 80.0;
    BiquadParameters mainsHighPass;
    BiquadParameters subLowPass;
    double confidence = 0.0;
    std::string summary;

    // Backward compatibility fields
    double delayMs = 0.0;
    int delaySamples = 0;
};

class SubwooferAssist {
public:
    static SubwooferRecommendation recommend(const ImpulseResponse& mainsIR, const ImpulseResponse& subIR);
};

#endif // SUBWOOFER_ASSIST_H
