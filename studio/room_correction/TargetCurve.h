#ifndef TARGET_CURVE_H
#define TARGET_CURVE_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

struct TargetBreakpoint {
    double freqHz = 1000.0;
    double gainDB = 0.0;

    TargetBreakpoint() = default;
    TargetBreakpoint(double f, double g) : freqHz(f), gainDB(g) {}

    bool operator==(const TargetBreakpoint& other) const { return freqHz == other.freqHz && gainDB == other.gainDB; }
};

enum class TargetPreset { Flat, BruelKjaer, Harman };

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
                double t = (logF - logLo) / (logHi - logLo);
                return lo.gainDB + t * (hi.gainDB - lo.gainDB);
            }
        }
        return breakpoints.back().gainDB;
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

    static TargetCurve getPreset(TargetPreset preset) {
        switch (preset) {
        case TargetPreset::Flat:
            return flat();
        case TargetPreset::BruelKjaer:
            return bruelKjaer();
        case TargetPreset::Harman:
            return harman();
        }
        return flat();
    }
};

#endif // TARGET_CURVE_H
