#ifndef SUBWOOFER_ASSIST_H
#define SUBWOOFER_ASSIST_H

#include "room_correction/ImpulseResponse.h"
#include <tuple>

struct SubwooferRecommendation {
    double delayMs = 0.0;
    int delaySamples = 0;
    double recommendedCrossoverHz = 80.0;
    bool invertSubPhase = false;
};

class SubwooferAssist {
public:
    static SubwooferRecommendation recommend(const ImpulseResponse& mainsIR, const ImpulseResponse& subIR);
};

#endif // SUBWOOFER_ASSIST_H
