#ifndef CALIBRATION_CURVE_H
#define CALIBRATION_CURVE_H

#include <optional>
#include <string>
#include <vector>

class CalibrationCurve {
public:
    std::vector<double> frequencies;
    std::vector<double> magnitudesDB;
    std::vector<double> phasesDeg;

    CalibrationCurve() = default;
    CalibrationCurve(const std::vector<double>& freqs, const std::vector<double>& mags,
                     const std::vector<double>& phases = {});

    double magnitude(double freqHz) const;
    double phase(double freqHz) const;

    std::vector<double> sampledMagnitudeDB(const std::vector<double>& grid) const;

    static std::optional<CalibrationCurve> parse(const std::string& text, const std::string& sourcePath = "<inline>");
    static std::optional<CalibrationCurve> load(const std::string& path);
    bool writeFRD(const std::string& path, const std::string& comment = "") const;

private:
    static double interpolate(double f, const std::vector<double>& freqs, const std::vector<double>& values);
};

#endif // CALIBRATION_CURVE_H
