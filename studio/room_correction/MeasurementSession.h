#ifndef MEASUREMENT_SESSION_H
#define MEASUREMENT_SESSION_H

#include "room_correction/ImpulseResponse.h"
#include "room_correction/FrequencyResponse.h"
#include "room_correction/TargetCurve.h"
#include "room_correction/CalibrationCurve.h"
#include "room_correction/PEQAutoFit.h"
#include "room_correction/FIRDesign.h"
#include "room_correction/SubwooferAssist.h"
#include "models/EQPreset.h"
#include "models/ConvolutionPreset.h"
#include <QObject>
#include <vector>
#include <string>
#include <optional>
#include <QUuid>

enum class MeasurementChannelKind {
    Full, Mains, Subwoofer
};
std::string channelKindToString(MeasurementChannelKind kind);

enum class FIRKind {
    MinimumPhase, LinearPhase, MeasurementDriven
};
std::string firKindToString(FIRKind kind);

enum class DisplaySmoothing {
    Off, Oct1over3, Oct1over6, Oct1over12, Oct1over24
};
std::string displaySmoothingToString(DisplaySmoothing s);

enum class FDWCycles {
    Off, Cycles1, Cycles5, Cycles10, Cycles15
};

struct MeasurementPosition {
    QUuid id;
    std::string name;
    FrequencyResponse fr;
    std::optional<ImpulseResponse> ir;
    bool isEnabled = true;
    MeasurementChannelKind kind = MeasurementChannelKind::Full;

    MeasurementPosition() : id(QUuid::createUuid()) {}
    MeasurementPosition(const std::string& name, const FrequencyResponse& fr, const std::optional<ImpulseResponse>& ir = std::nullopt, bool enabled = true, MeasurementChannelKind kind = MeasurementChannelKind::Full)
        : id(QUuid::createUuid()), name(name), fr(fr), ir(ir), isEnabled(enabled), kind(kind) {}
};

class MeasurementSession : public QObject {
    Q_OBJECT

public:
    explicit MeasurementSession(QObject* parent = nullptr);

    double sweepF1 = 20.0;
    double sweepF2 = 20000.0;
    double sweepDurationSeconds = 1.0;
    int sampleRate = 48000;

    TargetPreset targetPreset = TargetPreset::Flat;
    std::optional<TargetCurve> customTarget;
    TargetCurve targetCurve() const {
        if (customTarget.has_value() && !customTarget.value().breakpoints.empty()) {
            return customTarget.value();
        }
        return TargetCurve::getPreset(targetPreset);
    }

    int bandCount = 8;
    double maxGainDB = 12.0;
    bool modalMode = false;
    double schroederHz = 200.0;
    double modalMinQ = 2.0;

    FDWCycles fdwCycles = FDWCycles::Off;
    DisplaySmoothing displaySmoothing = DisplaySmoothing::Oct1over6;

    std::optional<CalibrationCurve> calibration;
    std::string calibrationPath;

    FIRKind firKind = FIRKind::MinimumPhase;
    int firTapCount = 8192;
    double firPhaseBlend = 1.0;

    std::vector<MeasurementPosition> positions;
    std::optional<ImpulseResponse> measuredIR;
    std::optional<FrequencyResponse> measuredFR;
    std::vector<double> measuredMagDB;
    std::vector<double> grid;

    std::optional<EQPreset> correctionPreset;
    std::string status = "No measurement loaded.";

    void generateMockMeasurement(bool append = false);
    void importPositionFRD(const std::string& path);

    void togglePosition(const QUuid& id);
    void removePosition(const QUuid& id);
    void setPositionKind(const QUuid& id, MeasurementChannelKind kind);

    void recomputeAverage();
    void runFit();
    std::optional<ConvolutionPreset> generateFIR(const std::vector<std::string>& existingNames);

    void loadCalibration(const std::string& path);
    void clearCalibration();
    bool exportFRD(const std::string& path, bool includeCalibration = false);

    bool subwooferAssistAvailable() const;
    std::optional<SubwooferRecommendation> computeSubwooferRecommendation();

    std::vector<double> displayedMagDB() const;
    void reset();

signals:
    void sessionUpdated();

private:
    std::vector<BiquadParameters> randomMockSystem();
};

#endif // MEASUREMENT_SESSION_H
