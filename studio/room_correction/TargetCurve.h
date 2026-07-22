#ifndef TARGET_CURVE_H
#define TARGET_CURVE_H

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

struct TargetBreakpoint {
    double freqHz = 1000.0;
    double gainDB = 0.0;

    TargetBreakpoint() = default;
    TargetBreakpoint(double f, double g) : freqHz(f), gainDB(g) {}

    bool operator==(const TargetBreakpoint& other) const { return freqHz == other.freqHz && gainDB == other.gainDB; }
};

enum class TargetPreset { Flat, BruelKjaer, Harman, Tilt, BassBoost };

class TargetCurve {
public:
    std::vector<TargetBreakpoint> breakpoints;

    TargetCurve() = default;
    explicit TargetCurve(const std::vector<TargetBreakpoint>& bps) { setBreakpoints(bps); }

    void setBreakpoints(const std::vector<TargetBreakpoint>& bps) {
        breakpoints = bps;
        std::sort(breakpoints.begin(), breakpoints.end(),
                  [](const TargetBreakpoint& a, const TargetBreakpoint& b) { return a.freqHz < b.freqHz; });
    }

    void upsert(const TargetBreakpoint& bp, double mergeToleranceHz = 1.0) {
        bool merged = false;
        for (auto& existing : breakpoints) {
            if (std::abs(existing.freqHz - bp.freqHz) <= mergeToleranceHz) {
                existing = bp;
                merged = true;
                break;
            }
        }
        if (!merged) {
            breakpoints.push_back(bp);
        }
        setBreakpoints(breakpoints);
    }

    double evaluate(double freqHz) const {
        if (breakpoints.empty())
            return 0.0;
        if (freqHz <= breakpoints.front().freqHz)
            return breakpoints.front().gainDB;
        if (freqHz >= breakpoints.back().freqHz)
            return breakpoints.back().gainDB;

        for (size_t i = 0; i < breakpoints.size() - 1; ++i) {
            const auto& lo = breakpoints[i];
            const auto& hi = breakpoints[i + 1];
            if (freqHz >= lo.freqHz && freqHz <= hi.freqHz) {
                double logF = std::log10(freqHz);
                double logLo = std::log10(lo.freqHz);
                double logHi = std::log10(hi.freqHz);
                double denom = logHi - logLo;
                if (denom < 1e-9)
                    return lo.gainDB;
                double t = (logF - logLo) / denom;
                return lo.gainDB + t * (hi.gainDB - lo.gainDB);
            }
        }
        return breakpoints.back().gainDB;
    }

    static std::optional<TargetCurve> parse(const std::string& text) {
        std::vector<TargetBreakpoint> bps;
        std::stringstream ss(text);
        std::string rawLine;
        while (std::getline(ss, rawLine)) {
            size_t first = rawLine.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                continue;
            std::string trimmed = rawLine.substr(first);
            if (trimmed.empty() || trimmed[0] == '*' || trimmed[0] == '#' || trimmed[0] == ';')
                continue;

            std::stringstream lineStream(trimmed);
            double f = 0.0, g = 0.0;
            if (lineStream >> f >> g) {
                bps.push_back(TargetBreakpoint(f, g));
            }
        }
        if (bps.empty())
            return std::nullopt;
        return TargetCurve(bps);
    }

    static std::optional<TargetCurve> load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open())
            return std::nullopt;
        std::stringstream buffer;
        buffer << file.rdbuf();
        return parse(buffer.str());
    }

    bool writeText(const std::string& path, const std::string& comment = "") const {
        std::ofstream file(path);
        if (!file.is_open())
            return false;
        file << "* Target Curve Data\n";
        file << "* Exported by DSPMonitor\n";
        if (!comment.empty()) {
            std::stringstream ss(comment);
            std::string line;
            while (std::getline(ss, line)) {
                file << "* " << line << "\n";
            }
        }
        file << "* Frequency [Hz]   Gain [dB]\n";
        char lineBuf[128];
        for (const auto& bp : breakpoints) {
            std::snprintf(lineBuf, sizeof(lineBuf), "%.6f %.6f\n", bp.freqHz, bp.gainDB);
            file << lineBuf;
        }
        return file.good();
    }

    static TargetCurve flat() { return TargetCurve({TargetBreakpoint(20.0, 0.0), TargetBreakpoint(20000.0, 0.0)}); }

    static TargetCurve bruelKjaer() {
        return TargetCurve({TargetBreakpoint(20.0, 3.0), TargetBreakpoint(50.0, 3.0), TargetBreakpoint(1000.0, 0.0),
                            TargetBreakpoint(4000.0, -2.0), TargetBreakpoint(16000.0, -4.0),
                            TargetBreakpoint(20000.0, -4.0)});
    }

    static TargetCurve harman() {
        return TargetCurve({TargetBreakpoint(20.0, 4.5), TargetBreakpoint(80.0, 4.5), TargetBreakpoint(200.0, 1.5),
                            TargetBreakpoint(1000.0, 0.0), TargetBreakpoint(10000.0, -3.0),
                            TargetBreakpoint(20000.0, -5.5)});
    }

    static TargetCurve tilt(double slopeDbPerOct = -1.0, double pivotHz = 1000.0) {
        double gain20 = slopeDbPerOct * (std::log10(20.0 / pivotHz) / std::log10(2.0));
        double gain20k = slopeDbPerOct * (std::log10(20000.0 / pivotHz) / std::log10(2.0));
        return TargetCurve({TargetBreakpoint(20.0, gain20), TargetBreakpoint(20000.0, gain20k)});
    }

    static TargetCurve bassBoost(double boostDB = 4.0, double cornerHz = 80.0, double transitionHz = 200.0) {
        return TargetCurve({TargetBreakpoint(20.0, boostDB), TargetBreakpoint(cornerHz, boostDB),
                            TargetBreakpoint(transitionHz, 0.0), TargetBreakpoint(20000.0, 0.0)});
    }

    static TargetCurve getPreset(TargetPreset preset) {
        switch (preset) {
        case TargetPreset::Flat:
            return flat();
        case TargetPreset::BruelKjaer:
            return bruelKjaer();
        case TargetPreset::Harman:
            return harman();
        case TargetPreset::Tilt:
            return tilt();
        case TargetPreset::BassBoost:
            return bassBoost();
        }
        return flat();
    }
};

#endif // TARGET_CURVE_H
