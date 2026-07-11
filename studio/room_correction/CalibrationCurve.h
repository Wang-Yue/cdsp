#ifndef CALIBRATION_CURVE_H
#define CALIBRATION_CURVE_H

#include <vector>
#include <string>
#include <optional>

class CalibrationCurve {
public:
    std::vector<double> frequencies;
    std::vector<double> magnitudesDB;
    std::vector<double> phasesDeg;

    CalibrationCurve() = default;
    CalibrationCurve(const std::vector<double>& freqs, const std::vector<double>& mags, const std::vector<double>& phases = {});

    double magnitude(double freqHz) const;
    double phase(double freqHz) const;

    static std::optional<CalibrationCurve> load(const std::string& path);
    bool writeFRD(const std::string& path, const std::string& comment = "") const;
};

#endif // CALIBRATION_CURVE_H
