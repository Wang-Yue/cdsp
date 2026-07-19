#include "room_correction/CalibrationCurve.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

CalibrationCurve::CalibrationCurve(const std::vector<double>& freqs, const std::vector<double>& mags,
                                   const std::vector<double>& phases)
    : frequencies(freqs), magnitudesDB(mags), phasesDeg(phases) {
    bool hasPhases = !phasesDeg.empty() && phasesDeg.size() == frequencies.size();
    if (!hasPhases) {
        phasesDeg.clear();
    }
}

double CalibrationCurve::interpolate(double f, const std::vector<double>& frequencies,
                                     const std::vector<double>& values) {
    size_t n = frequencies.size();
    if (n == 0 || f <= 0.0)
        return 0.0;
    if (n == 1)
        return values[0];
    if (f <= frequencies[0])
        return values[0];
    if (f >= frequencies[n - 1])
        return values[n - 1];

    size_t lo = 0;
    size_t hi = n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (frequencies[mid] <= f)
            lo = mid;
        else
            hi = mid;
    }

    double fLo = std::max(1e-3, frequencies[lo]);
    double fHi = std::max(fLo + 1e-3, frequencies[hi]);
    double vLo = values[lo];
    double vHi = values[hi];

    double logF = std::log10(std::max(1e-3, f));
    double logLo = std::log10(fLo);
    double logHi = std::log10(fHi);
    double denom = logHi - logLo;
    double t = (denom > 1e-9) ? (logF - logLo) / denom : 0.0;
    return vLo + t * (vHi - vLo);
}

double CalibrationCurve::magnitude(double freqHz) const {
    return interpolate(freqHz, frequencies, magnitudesDB);
}

double CalibrationCurve::phase(double freqHz) const {
    if (phasesDeg.empty())
        return 0.0;
    return interpolate(freqHz, frequencies, phasesDeg);
}

std::vector<double> CalibrationCurve::sampledMagnitudeDB(const std::vector<double>& grid) const {
    std::vector<double> result(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) {
        result[i] = magnitude(grid[i]);
    }
    return result;
}

std::optional<CalibrationCurve> CalibrationCurve::parse(const std::string& text, const std::string& sourcePath) {
    (void)sourcePath;
    std::vector<double> freqs, mags, phases;
    bool sawPhaseColumn = false;

    std::stringstream ss(text);
    std::string rawLine;
    while (std::getline(ss, rawLine)) {
        size_t first = rawLine.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        std::string trimmed = rawLine.substr(first);
        size_t last = trimmed.find_last_not_of(" \t\r\n");
        if (last != std::string::npos)
            trimmed = trimmed.substr(0, last + 1);

        if (trimmed.empty())
            continue;
        if (trimmed[0] == '*' || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        std::stringstream lineStream(trimmed);
        std::vector<std::string> fields;
        std::string token;
        while (lineStream >> token) {
            fields.push_back(token);
        }

        if (fields.size() < 2)
            continue;

        double f = 0.0, m = 0.0;
        try {
            f = std::stod(fields[0]);
            m = std::stod(fields[1]);
        } catch (...) {
            continue;
        }

        freqs.push_back(f);
        mags.push_back(m);

        if (fields.size() >= 3) {
            try {
                double p = std::stod(fields[2]);
                phases.push_back(p);
                sawPhaseColumn = true;
            } catch (...) {
                if (sawPhaseColumn)
                    phases.push_back(0.0);
            }
        } else if (sawPhaseColumn) {
            phases.push_back(0.0);
        }
    }

    if (freqs.empty())
        return std::nullopt;

    bool isDescending = false;
    for (size_t i = 1; i < freqs.size(); ++i) {
        if (freqs[i] < freqs[i - 1]) {
            isDescending = true;
            break;
        }
    }

    if (isDescending) {
        struct Point {
            double f, m;
        };
        std::vector<Point> pts(freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i) {
            pts[i] = {freqs[i], mags[i]};
        }
        std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.f < b.f; });
        for (size_t i = 0; i < pts.size(); ++i) {
            freqs[i] = pts[i].f;
            mags[i] = pts[i].m;
        }
        sawPhaseColumn = false;
        phases.clear();
    }

    return CalibrationCurve(freqs, mags, sawPhaseColumn ? phases : std::vector<double>{});
}

std::optional<CalibrationCurve> CalibrationCurve::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return std::nullopt;

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str(), path);
}

bool CalibrationCurve::writeFRD(const std::string& path, const std::string& comment) const {
    std::ofstream file(path);
    if (!file.is_open())
        return false;

    file << "* Frequency Response Data\n";
    file << "* Exported by DSPMonitor\n";

    if (!comment.empty()) {
        std::stringstream ss(comment);
        std::string line;
        while (std::getline(ss, line)) {
            file << "* " << line << "\n";
        }
    }

    bool hasPhase = !phasesDeg.empty() && phasesDeg.size() == frequencies.size();
    if (hasPhase) {
        file << "* Frequency [Hz]   Magnitude [dB]   Phase [deg]\n";
    } else {
        file << "* Frequency [Hz]   Magnitude [dB]\n";
    }

    char lineBuf[256];
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (hasPhase) {
            std::snprintf(lineBuf, sizeof(lineBuf), "%.6f %.6f %.6f\n", frequencies[i], magnitudesDB[i], phasesDeg[i]);
        } else {
            std::snprintf(lineBuf, sizeof(lineBuf), "%.6f %.6f\n", frequencies[i], magnitudesDB[i]);
        }
        file << lineBuf;
    }

    return file.good();
}
