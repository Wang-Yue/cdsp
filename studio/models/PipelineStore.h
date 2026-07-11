#ifndef PIPELINE_STORE_H
#define PIPELINE_STORE_H

#include "models/ConvolutionPreset.h"
#include "models/EQPreset.h"
#include "models/PipelineStage.h"

#include <QObject>
#include <QUuid>
#include <map>
#include <vector>

class PipelineStore : public QObject {
    Q_OBJECT

public:
    explicit PipelineStore(QObject* parent = nullptr);

    std::vector<PipelineStage> stages;
    std::vector<EQPreset> eqPresets;
    std::vector<ConvolutionPreset> convPresets;

    QUuid addStage(StageType type);
    QUuid duplicateStage(const QUuid& id);
    int channelCountBeforeStage(size_t index, int captureChannels) const;
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
