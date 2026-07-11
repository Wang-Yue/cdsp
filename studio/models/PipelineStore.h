#ifndef PIPELINE_STORE_H
#define PIPELINE_STORE_H

#include "models/PipelineStage.h"
#include "models/EQPreset.h"
#include "models/ConvolutionPreset.h"
#include <QObject>
#include <vector>
#include <map>
#include <QUuid>

class PipelineStore : public QObject {
    Q_OBJECT

public:
    explicit PipelineStore(QObject* parent = nullptr);

    std::vector<PipelineStage> stages;
    std::vector<EQPreset> eqPresets;
    std::vector<ConvolutionPreset> convPresets;

    void addStage(StageType type);
    void deleteStage(const QUuid& id);
    void moveStage(int from, int to);

    QUuid addEQPreset(const EQPreset& preset = EQPreset("New EQ Preset"));
    void updateEQPreset(const EQPreset& preset);
    void deleteEQPreset(const QUuid& id);

    QUuid addConvPreset(const ConvolutionPreset& preset);
    void updateConvPreset(const ConvolutionPreset& preset);
    void deleteConvPreset(const QUuid& id);

    StageBuildResult buildPipeline(int sampleRate, int channelCount) const;

    void load();
    void save();

signals:
    void pipelineChanged();

private:
    void ensureDefaultPresets();
};

#endif // PIPELINE_STORE_H
