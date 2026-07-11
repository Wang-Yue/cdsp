#ifndef FIR_DESIGN_H
#define FIR_DESIGN_H

#include "config/BiquadCoefficients.h"
#include "room_correction/FrequencyResponse.h"
#include "room_correction/TargetCurve.h"

#include <vector>

struct FIRDesignOptions {
    int fftSize = 8192;
    int outputLength = 8192;
    double preampDB = 0.0;
    double floorDB = -80.0;
};

struct FIRDesignMeasurementOptions {
    int fftSize = 8192;
    double floorDB = -60.0;
    double preampDB = -6.0;
    double maxBoostDB = 12.0;
    double minFreqHz = 30.0;
    double maxFreqHz = 18000.0;
    double phaseBlend = 1.0; // 0 = min-phase, 1 = linear-phase
};

class FIRDesign {
public:
    static std::vector<double> minimumPhase(const std::vector<BiquadParameters>& bands, int sampleRate,
                                            const FIRDesignOptions& options = FIRDesignOptions());

    static std::vector<double> linearPhase(const std::vector<BiquadParameters>& bands, int sampleRate,
                                           const FIRDesignOptions& options = FIRDesignOptions());

    static std::vector<double> minimumPhaseFromMagDB(const std::vector<double>& magDB, int sampleRate,
                                                     const FIRDesignOptions& options = FIRDesignOptions());

    static std::vector<double> linearPhaseFromMagDB(const std::vector<double>& magDB, int sampleRate,
                                                    const FIRDesignOptions& options = FIRDesignOptions());

    static std::vector<double>
    fromMeasurement(const FrequencyResponse& measured, const TargetCurve& target, int sampleRate,
                    const FIRDesignMeasurementOptions& options = FIRDesignMeasurementOptions());
};

#endif // FIR_DESIGN_H
