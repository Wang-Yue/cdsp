#include "room_correction/MeasurementSession.h"
#include "room_correction/SweepGenerator.h"
#include "room_correction/SweepDeconvolver.h"
#include "models/ConvCoefficientLoader.h"
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <cmath>
#include <random>
#include <algorithm>

std::string channelKindToString(MeasurementChannelKind kind) {
    switch (kind) {
    case MeasurementChannelKind::Full: return "Full Range";
    case MeasurementChannelKind::Mains: return "Mains Only";
    case MeasurementChannelKind::Subwoofer: return "Subwoofer Only";
    }
    return "Full Range";
}

std::string firKindToString(FIRKind kind) {
    switch (kind) {
    case FIRKind::MinimumPhase: return "Min-phase";
    case FIRKind::LinearPhase: return "Linear-phase";
    case FIRKind::MeasurementDriven: return "From measurement";
    }
    return "Min-phase";
}

std::string displaySmoothingToString(DisplaySmoothing s) {
    switch (s) {
    case DisplaySmoothing::Off: return "Off";
    case DisplaySmoothing::Oct1over3: return "1/3 oct";
    case DisplaySmoothing::Oct1over6: return "1/6 oct";
    case DisplaySmoothing::Oct1over12: return "1/12 oct";
    case DisplaySmoothing::Oct1over24: return "1/24 oct";
    }
    return "1/6 oct";
}

MeasurementSession::MeasurementSession(QObject* parent) : QObject(parent) {}

void MeasurementSession::reset() {
    positions.clear();
    measuredIR = std::nullopt;
    measuredFR = std::nullopt;
    measuredMagDB.clear();
    grid.clear();
    correctionPreset = std::nullopt;
    status = "No measurement loaded.";
    emit sessionUpdated();
}

std::vector<double> MeasurementSession::displayedMagDB() const {
    if (measuredMagDB.empty()) return {};
    double octaves = 0.0;
    switch (displaySmoothing) {
    case DisplaySmoothing::Off: return measuredMagDB;
    case DisplaySmoothing::Oct1over3: octaves = 1.0 / 3.0; break;
    case DisplaySmoothing::Oct1over6: octaves = 1.0 / 6.0; break;
    case DisplaySmoothing::Oct1over12: octaves = 1.0 / 12.0; break;
    case DisplaySmoothing::Oct1over24: octaves = 1.0 / 24.0; break;
    }
    return PEQAutoFit::smoothLogOctave(measuredMagDB, grid, octaves);
}

void MeasurementSession::generateMockMeasurement(bool append) {
    status = "Generating mock measurement…";
    if (!append) positions.clear();

    auto [sweep, inv] = SweepGenerator::sweepAndInverse(sweepF1, sweepF2, sweepDurationSeconds, sampleRate);
    std::vector<double> captured = sweep;

    // Apply mock room biquads
    BiquadParameters hp; hp.type = BiquadType::Highpass; hp.freq = 35.0; hp.q = 0.707;
    BiquadParameters mode1; mode1.type = BiquadType::Peaking; mode1.freq = 60.0; mode1.gain = 6.0; mode1.q = 4.0;
    BiquadParameters mode2; mode2.type = BiquadType::Peaking; mode2.freq = 120.0; mode2.gain = -8.0; mode2.q = 3.0;

    for (const auto& p : {hp, mode1, mode2}) {
        auto coeffs = BiquadCoefficients::compute(p, sampleRate);
        if (coeffs.has_value()) {
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            const auto& c = coeffs.value();
            for (size_t i = 0; i < captured.size(); ++i) {
                double x = captured[i];
                double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                captured[i] = y;
            }
        }
    }

    ImpulseResponse ir = SweepDeconvolver::deconvolve(captured, sweepF1, sweepF2, sweepDurationSeconds, sampleRate);
    ImpulseResponse windowed = ir.windowed(sampleRate / 200, sampleRate / 5, 0.1);
    FrequencyResponse fr = FrequencyResponse::from(windowed);

    std::string name = "Position " + std::to_string(positions.size() + 1);
    positions.push_back(MeasurementPosition(name, fr, windowed));

    recomputeAverage();
    if (!append) {
        correctionPreset = EQPreset("Room Correction", 0.0, {});
    }
    status = "Mock measurement ready (" + std::to_string(positions.size()) + " positions).";
    emit sessionUpdated();
}

void MeasurementSession::importPositionFRD(const std::string& path) {
    auto cal = CalibrationCurve::load(path);
    if (!cal.has_value()) {
        status = "FRD import failed: could not parse file.";
        emit sessionUpdated();
        return;
    }

    int fftSize = 4096;
    size_t bins = fftSize / 2 + 1;
    std::vector<double> re(bins), im(bins);
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);

    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double dB = cal.value().magnitude(std::max(1.0, f));
        double mag = std::pow(10.0, dB / 20.0);
        double phaseDeg = cal.value().phase(std::max(1.0, f));
        double phaseRad = phaseDeg * M_PI / 180.0;
        re[k] = mag * std::cos(phaseRad);
        im[k] = mag * std::sin(phaseRad);
    }

    FrequencyResponse fr(re, im, sampleRate, fftSize);
    QFileInfo fi(QString::fromStdString(path));
    positions.push_back(MeasurementPosition(fi.fileName().toStdString(), fr, std::nullopt));

    recomputeAverage();
    if (!correctionPreset.has_value()) {
        correctionPreset = EQPreset("Room Correction", 0.0, {});
    }
    status = "Imported " + fi.fileName().toStdString() + ".";
    emit sessionUpdated();
}

void MeasurementSession::recomputeAverage() {
    std::vector<MeasurementPosition*> enabled;
    for (auto& p : positions) if (p.isEnabled) enabled.push_back(&p);

    if (enabled.empty()) {
        measuredFR = std::nullopt;
        measuredIR = std::nullopt;
        measuredMagDB.clear();
        grid.clear();
        emit sessionUpdated();
        return;
    }

    grid = PEQAutoFit::logFrequencyGrid(20.0, 20000.0, 256);
    std::vector<double> combinedDB(grid.size(), 0.0);

    if (enabled.size() == 1) {
        combinedDB = PEQAutoFit::sampleMagnitudeDB(enabled[0]->fr, grid);
    } else {
        std::vector<double> sumPow(grid.size(), 0.0);
        for (const auto* p : enabled) {
            auto dB = PEQAutoFit::sampleMagnitudeDB(p->fr, grid);
            for (size_t i = 0; i < grid.size(); ++i) {
                double lin = std::pow(10.0, dB[i] / 20.0);
                sumPow[i] += lin * lin;
            }
        }
        double n = static_cast<double>(enabled.size());
        for (size_t i = 0; i < grid.size(); ++i) {
            double mean = std::sqrt(sumPow[i] / n);
            combinedDB[i] = 20.0 * std::log10(std::max(mean, 1e-12));
        }
    }

    // Calibration subtraction
    if (calibration.has_value()) {
        for (size_t i = 0; i < grid.size(); ++i) {
            combinedDB[i] -= calibration.value().magnitude(grid[i]);
        }
    }

    // Median level normalization
    std::vector<double> inBand;
    for (size_t i = 0; i < grid.size(); ++i) {
        if (grid[i] >= 200.0 && grid[i] <= 5000.0) inBand.push_back(combinedDB[i]);
    }
    if (!inBand.empty()) {
        std::sort(inBand.begin(), inBand.end());
        double median = inBand[inBand.size() / 2];
        for (size_t i = 0; i < grid.size(); ++i) combinedDB[i] -= median;
    }

    measuredFR = enabled[0]->fr;
    measuredIR = enabled.back()->ir;
    measuredMagDB = combinedDB;

    emit sessionUpdated();
}

void MeasurementSession::togglePosition(const QUuid& id) {
    for (auto& p : positions) {
        if (p.id == id) {
            p.isEnabled = !p.isEnabled;
            break;
        }
    }
    recomputeAverage();
}

void MeasurementSession::removePosition(const QUuid& id) {
    positions.erase(std::remove_if(positions.begin(), positions.end(), [&id](const MeasurementPosition& p) {
        return p.id == id;
    }), positions.end());

    if (positions.empty()) reset();
    else recomputeAverage();
}

void MeasurementSession::setPositionKind(const QUuid& id, MeasurementChannelKind kind) {
    for (auto& p : positions) {
        if (p.id == id) {
            p.kind = kind;
            break;
        }
    }
    emit sessionUpdated();
}

void MeasurementSession::runFit() {
    if (measuredMagDB.empty()) return;

    PEQAutoFitOptions opts;
    opts.bandCount = bandCount;
    opts.maxGainDB = maxGainDB;
    opts.modalMode = modalMode;
    opts.schroederHz = schroederHz;
    opts.modalMinQ = modalMinQ;

    auto biquadParams = PEQAutoFit::fit(measuredMagDB, grid, targetCurve(), sampleRate, opts);

    std::vector<EQBand> eqBands;
    for (const auto& bp : biquadParams) {
        EQBand band(EQBandType::Peaking, bp.freq.value_or(1000.0), bp.gain.value_or(0.0), bp.q.value_or(0.707));
        eqBands.push_back(band);
    }

    correctionPreset = EQPreset("Room Correction", -6.0, eqBands);
    status = "Fit produced " + std::to_string(eqBands.size()) + " bands.";
    emit sessionUpdated();
}

std::optional<ConvolutionPreset> MeasurementSession::generateFIR(const std::vector<std::string>& existingNames) {
    if (!correctionPreset.has_value() || correctionPreset.value().bands.empty()) {
        status = "Run PEQ fit before generating FIR.";
        emit sessionUpdated();
        return std::nullopt;
    }

    std::vector<BiquadParameters> bands;
    for (const auto& b : correctionPreset.value().bands) {
        if (!b.isEnabled) continue;
        BiquadParameters p;
        p.type = stringToBiquadType(eqBandTypeToString(b.type));
        p.freq = b.freq; p.gain = b.gain; p.q = b.q;
        bands.push_back(p);
    }

    QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir irDir(appDataDir + "/IRs");
    if (!irDir.exists()) irDir.mkpath(".");

    std::map<int, std::string> irPaths;
    QUuid presetId = QUuid::createUuid();

    std::vector<int> rates = {44100, 48000, 88200, 96000, 192000};
    for (int rate : rates) {
        FIRDesignOptions opts;
        opts.fftSize = firTapCount;
        opts.outputLength = firTapCount;
        opts.preampDB = 0.0;

        std::vector<double> irSamples = (firKind == FIRKind::MinimumPhase)
            ? FIRDesign::minimumPhase(bands, rate, opts)
            : FIRDesign::linearPhase(bands, rate, opts);

        QString fileName = QString("RoomCorrection-%1-%2-%3.f64")
            .arg(QString::fromStdString(firKindToString(firKind)))
            .arg(rate)
            .arg(presetId.toString(QUuid::WithoutBraces).left(8));

        QString fullPath = irDir.filePath(fileName);
        ConvCoefficientLoader::saveRawFloat64(irSamples, fullPath.toStdString());
        irPaths[rate] = fullPath.toStdString();
    }

    std::string presetName = "Room Correction (" + firKindToString(firKind) + ")";
    ConvolutionPreset preset(presetName, irPaths, firTapCount, firKindToString(firKind));

    status = "Generated FIR preset: " + presetName;
    emit sessionUpdated();
    return preset;
}

void MeasurementSession::loadCalibration(const std::string& path) {
    auto cal = CalibrationCurve::load(path);
    if (cal.has_value()) {
        calibration = cal;
        calibrationPath = path;
        recomputeAverage();
        status = "Loaded calibration curve.";
    } else {
        status = "Calibration load failed.";
    }
    emit sessionUpdated();
}

void MeasurementSession::clearCalibration() {
    calibration = std::nullopt;
    calibrationPath.clear();
    recomputeAverage();
    status = "Calibration cleared.";
    emit sessionUpdated();
}

bool MeasurementSession::exportFRD(const std::string& path, bool includeCalibration) {
    if (!measuredFR.has_value()) return false;
    const auto& fr = measuredFR.value();

    std::vector<double> freqs, mags, phases;
    double binHz = static_cast<double>(fr.sampleRate) / static_cast<double>(fr.fftSize);

    for (size_t k = 1; k < fr.bins(); ++k) {
        double f = static_cast<double>(k) * binHz;
        if (f < 20.0 || f > 20000.0) continue;
        freqs.push_back(f);

        double m = fr.magnitudeDB(k);
        if (includeCalibration && calibration.has_value()) {
            m -= calibration.value().magnitude(f);
        }
        mags.push_back(m);
        phases.push_back(fr.phase(k) * 180.0 / M_PI);
    }

    CalibrationCurve expCurve(freqs, mags, phases);
    return expCurve.writeFRD(path, "Sample Rate: " + std::to_string(sampleRate));
}

bool MeasurementSession::subwooferAssistAvailable() const {
    bool hasMains = false, hasSub = false;
    for (const auto& p : positions) {
        if (p.kind == MeasurementChannelKind::Mains && p.ir.has_value()) hasMains = true;
        if (p.kind == MeasurementChannelKind::Subwoofer && p.ir.has_value()) hasSub = true;
    }
    return hasMains && hasSub;
}

std::optional<SubwooferRecommendation> MeasurementSession::computeSubwooferRecommendation() {
    const ImpulseResponse* mainsIR = nullptr;
    const ImpulseResponse* subIR = nullptr;

    for (const auto& p : positions) {
        if (p.kind == MeasurementChannelKind::Mains && p.ir.has_value()) mainsIR = &p.ir.value();
        if (p.kind == MeasurementChannelKind::Subwoofer && p.ir.has_value()) subIR = &p.ir.value();
    }

    if (!mainsIR || !subIR) return std::nullopt;
    return SubwooferAssist::recommend(*mainsIR, *subIR);
}
