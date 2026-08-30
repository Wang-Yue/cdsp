#ifndef PIPELINE_STORE_H
#define PIPELINE_STORE_H

#include "models/ConvolutionPreset.h" // for ConvolutionPreset
#include "models/EQPreset.h"          // for EQPreset, EQBand
#include "models/PipelineStage.h"     // for PipelineStage, StageBuildResult, StageType

#include <QByteArray>  // for QByteArray
#include <QJsonObject> // for QJsonObject
#include <QObject>     // for QObject, Q_OBJECT, signals
#include <QString>     // for QString
#include <QUuid>       // for QUuid
#include <optional>    // for optional, nullopt, nullopt_t
#include <stddef.h>    // for size_t
#include <string>      // for basic_string, string
#include <vector>      // for vector

struct PipelineStoreSnapshot {
    std::vector<PipelineStage> stages;
    std::vector<EQPreset> eqPresets;
    std::vector<ConvolutionPreset> convPresets;
};

class PipelineStore : public QObject {
    Q_OBJECT

public:
    explicit PipelineStore(QObject* parent = nullptr);

    std::vector<PipelineStage> stages;
    std::vector<EQPreset> eqPresets;
    std::vector<ConvolutionPreset> convPresets;

    // MARK: - Pipeline Stage Management & Persistence
    void savePipelineStages();
    void loadPipelineStages();
    QUuid addStage(StageType type);
    QUuid duplicateStage(const QUuid& id);
    int channelCountBeforeStage(size_t index, int captureChannels) const;
    int incomingChannels(const QUuid& stageID, int captureChannels) const;
    void deleteStage(const QUuid& id);
    void deleteStage(size_t index);
    void moveStage(int from, int to);

    // MARK: - EQ Preset Persistence & Management
    void saveEQPresets();
    std::vector<EQPreset> loadEQPresets();
    QUuid addEQPreset(const std::string& name = "New Preset", double preamp = -6.0,
                      const std::optional<std::vector<EQBand>>& bands = std::nullopt);
    QUuid addEQPreset(const EQPreset& preset);
    void updateEQPreset(const EQPreset& preset);
    void deleteEQPreset(size_t index);
    void deleteEQPreset(const QUuid& id);

    // MARK: - Convolution Preset Persistence & Management
    void saveConvPresets();
    std::vector<ConvolutionPreset> loadConvPresets();
    QUuid addConvPreset(const ConvolutionPreset& preset);
    QUuid addConvolutionPreset(const ConvolutionPreset& preset);
    void updateConvPreset(const ConvolutionPreset& preset);
    void updateConvPreset();
    void deleteConvPreset(size_t index);
    void deleteConvPreset(const QUuid& id);

    // MARK: - Full Persistence
    void load();
    void save();

    // MARK: - Undo / Redo Snapshot Stack
    void pushUndoSnapshot();
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void clearUndoStack();

    // MARK: - JSON File Import / Export
    QJsonObject toJson() const;
    void restoreFromJson(const QJsonObject& json);
    QByteArray exportJson() const;
    bool exportToJsonFile(const QString& filePath) const;
    bool importJson(const QByteArray& jsonData);
    bool importFromJsonFile(const QString& filePath);

    StageBuildResult buildPipeline(int sampleRate, int channelCount) const;

signals:
    void pipelineChanged();

private:
    void ensureDefaultPresets();

    std::vector<PipelineStoreSnapshot> m_undoStack;
    std::vector<PipelineStoreSnapshot> m_redoStack;
    static constexpr size_t kMaxUndoStackSize = 50;
};

#endif // PIPELINE_STORE_H
