#include "models/PipelineStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

PipelineStore::PipelineStore(QObject* parent) : QObject(parent) {
    load();
}

void PipelineStore::ensureDefaultPresets() {
    if (stages.empty()) {
        stages = PipelineStage::defaultStages();
    }
}

// MARK: - Pipeline Stage Management & Persistence

void PipelineStore::savePipelineStages() {
    QSettings s("DSPMonitor", "MonitorQt");
    QJsonArray stagesArr;
    for (const auto& st : stages)
        stagesArr.append(st.toJson());
    s.setValue("pipelineStages", QJsonDocument(stagesArr).toJson(QJsonDocument::Compact));
}

void PipelineStore::loadPipelineStages() {
    QSettings s("DSPMonitor", "MonitorQt");
    QString stagesKey = s.contains("pipelineStages") ? "pipelineStages" : (s.contains("stages") ? "stages" : "");
    if (!stagesKey.isEmpty()) {
        stages.clear();
        QJsonArray arr = QJsonDocument::fromJson(s.value(stagesKey).toByteArray()).array();
        for (const auto& item : arr)
            stages.push_back(PipelineStage::fromJson(item.toObject()));
    }
    if (stages.empty()) {
        stages = PipelineStage::defaultStages();
    }
}

QUuid PipelineStore::addStage(StageType type) {
    pushUndoSnapshot();
    PipelineStage stage(type, "", true);
    stages.push_back(stage);
    savePipelineStages();
    emit pipelineChanged();
    return stage.id;
}

QUuid PipelineStore::duplicateStage(const QUuid& id) {
    auto it = std::find_if(stages.begin(), stages.end(), [&id](const PipelineStage& s) { return s.id == id; });
    if (it != stages.end()) {
        pushUndoSnapshot();
        PipelineStage dup = *it;
        dup.id = QUuid::createUuid();
        dup.name = dup.name + " (Copy)";
        stages.insert(it + 1, dup);
        savePipelineStages();
        emit pipelineChanged();
        return dup.id;
    }
    return QUuid();
}

int PipelineStore::channelCountBeforeStage(size_t index, int captureChannels) const {
    int current = captureChannels;
    for (size_t i = 0; i < index && i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (stage.isEnabled && stage.isActive() && stage.type == StageType::MatrixMixer) {
            current = stage.mixerChannelsOut;
        }
    }
    return current;
}

int PipelineStore::incomingChannels(const QUuid& stageID, int captureChannels) const {
    int count = captureChannels;
    for (const auto& stage : stages) {
        if (stage.id == stageID) {
            return count;
        }
        if (stage.isActive() && stage.type == StageType::MatrixMixer) {
            count = stage.mixerChannelsOut;
        }
    }
    return count;
}

void PipelineStore::deleteStage(const QUuid& id) {
    pushUndoSnapshot();
    stages.erase(std::remove_if(stages.begin(), stages.end(), [&id](const PipelineStage& s) { return s.id == id; }),
                 stages.end());
    savePipelineStages();
    emit pipelineChanged();
}

void PipelineStore::deleteStage(size_t index) {
    if (index >= stages.size())
        return;
    pushUndoSnapshot();
    stages.erase(stages.begin() + index);
    savePipelineStages();
    emit pipelineChanged();
}

void PipelineStore::moveStage(int from, int to) {
    if (from < 0 || from >= static_cast<int>(stages.size()))
        return;
    if (to < 0 || to >= static_cast<int>(stages.size()))
        return;
    pushUndoSnapshot();
    PipelineStage stage = stages[from];
    stages.erase(stages.begin() + from);
    stages.insert(stages.begin() + to, stage);
    savePipelineStages();
    emit pipelineChanged();
}

// MARK: - EQ Preset Persistence & Management

void PipelineStore::saveEQPresets() {
    QSettings s("DSPMonitor", "MonitorQt");
    QJsonArray eqArr;
    for (const auto& eq : eqPresets)
        eqArr.append(eq.toJson());
    s.setValue("eqPresets", QJsonDocument(eqArr).toJson(QJsonDocument::Compact));
}

std::vector<EQPreset> PipelineStore::loadEQPresets() {
    QSettings s("DSPMonitor", "MonitorQt");
    if (!s.contains("eqPresets")) {
        return {};
    }
    std::vector<EQPreset> list;
    QJsonArray arr = QJsonDocument::fromJson(s.value("eqPresets").toByteArray()).array();
    for (const auto& item : arr) {
        list.push_back(EQPreset::fromJson(item.toObject()));
    }
    return list;
}

QUuid PipelineStore::addEQPreset(const std::string& name, double preamp,
                                 const std::optional<std::vector<EQBand>>& bands) {
    pushUndoSnapshot();
    std::vector<EQBand> presetBands;
    if (bands.has_value()) {
        presetBands = bands.value();
    } else {
        presetBands = {EQBand(EQBandType::Peaking, 100.0, 0.0, 1.0), EQBand(EQBandType::Peaking, 1000.0, 0.0, 1.0),
                       EQBand(EQBandType::Peaking, 10000.0, 0.0, 1.0)};
    }
    EQPreset preset(name, preamp, presetBands);
    eqPresets.push_back(preset);
    saveEQPresets();
    emit pipelineChanged();
    return preset.id;
}

QUuid PipelineStore::addEQPreset(const EQPreset& preset) {
    pushUndoSnapshot();
    eqPresets.push_back(preset);
    saveEQPresets();
    emit pipelineChanged();
    return preset.id;
}

void PipelineStore::updateEQPreset(const EQPreset& preset) {
    pushUndoSnapshot();
    for (auto& p : eqPresets) {
        if (p.id == preset.id) {
            p = preset;
            break;
        }
    }
    saveEQPresets();
    emit pipelineChanged();
}

void PipelineStore::deleteEQPreset(size_t index) {
    if (index >= eqPresets.size())
        return;
    pushUndoSnapshot();
    QUuid toDeleteId = eqPresets[index].id;
    for (auto& stage : stages) {
        if (stage.eqPresetId == toDeleteId) {
            stage.eqPresetId = std::nullopt;
        }
    }
    eqPresets.erase(eqPresets.begin() + index);
    saveEQPresets();
    savePipelineStages();
    emit pipelineChanged();
}

void PipelineStore::deleteEQPreset(const QUuid& id) {
    auto it = std::find_if(eqPresets.begin(), eqPresets.end(), [&id](const EQPreset& p) { return p.id == id; });
    if (it != eqPresets.end()) {
        size_t idx = std::distance(eqPresets.begin(), it);
        deleteEQPreset(idx);
    }
}

// MARK: - Convolution Preset Persistence & Management

void PipelineStore::saveConvPresets() {
    QSettings s("DSPMonitor", "MonitorQt");
    QJsonArray convArr;
    for (const auto& conv : convPresets)
        convArr.append(conv.toJson());
    s.setValue("convPresets", QJsonDocument(convArr).toJson(QJsonDocument::Compact));
}

std::vector<ConvolutionPreset> PipelineStore::loadConvPresets() {
    QSettings s("DSPMonitor", "MonitorQt");
    if (!s.contains("convPresets")) {
        return {};
    }
    std::vector<ConvolutionPreset> list;
    QJsonArray arr = QJsonDocument::fromJson(s.value("convPresets").toByteArray()).array();
    for (const auto& item : arr) {
        list.push_back(ConvolutionPreset::fromJson(item.toObject()));
    }
    return list;
}

QUuid PipelineStore::addConvPreset(const ConvolutionPreset& preset) {
    pushUndoSnapshot();
    convPresets.push_back(preset);
    saveConvPresets();
    return preset.id;
}

QUuid PipelineStore::addConvolutionPreset(const ConvolutionPreset& preset) {
    return addConvPreset(preset);
}

void PipelineStore::updateConvPreset(const ConvolutionPreset& preset) {
    pushUndoSnapshot();
    for (auto& p : convPresets) {
        if (p.id == preset.id) {
            p = preset;
            break;
        }
    }
    saveConvPresets();
    emit pipelineChanged();
}

void PipelineStore::updateConvPreset() {
    saveConvPresets();
    emit pipelineChanged();
}

void PipelineStore::deleteConvPreset(size_t index) {
    if (index >= convPresets.size())
        return;
    pushUndoSnapshot();
    const auto& toDelete = convPresets[index];
    for (auto& stage : stages) {
        if (stage.convPresetId == toDelete.id) {
            stage.convPresetId = std::nullopt;
        }
    }
    for (const auto& [rate, path] : toDelete.irPaths) {
        if (!path.empty()) {
            QFile::remove(QString::fromStdString(path));
        }
    }
    convPresets.erase(convPresets.begin() + index);
    saveConvPresets();
    savePipelineStages();
    emit pipelineChanged();
}

void PipelineStore::deleteConvPreset(const QUuid& id) {
    auto it =
        std::find_if(convPresets.begin(), convPresets.end(), [&id](const ConvolutionPreset& p) { return p.id == id; });
    if (it != convPresets.end()) {
        size_t idx = std::distance(convPresets.begin(), it);
        deleteConvPreset(idx);
    }
}

// MARK: - Full Persistence

void PipelineStore::load() {
    eqPresets = loadEQPresets();
    convPresets = loadConvPresets();
    loadPipelineStages();
}

void PipelineStore::save() {
    savePipelineStages();
    saveEQPresets();
    saveConvPresets();
}

// MARK: - Undo / Redo Snapshot Stack

void PipelineStore::pushUndoSnapshot() {
    m_undoStack.push_back({stages, eqPresets, convPresets});
    if (m_undoStack.size() > kMaxUndoStackSize) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}

bool PipelineStore::canUndo() const {
    return !m_undoStack.empty();
}

bool PipelineStore::canRedo() const {
    return !m_redoStack.empty();
}

void PipelineStore::undo() {
    if (m_undoStack.empty())
        return;
    m_redoStack.push_back({stages, eqPresets, convPresets});
    auto snap = m_undoStack.back();
    m_undoStack.pop_back();
    stages = snap.stages;
    eqPresets = snap.eqPresets;
    convPresets = snap.convPresets;
    save();
    emit pipelineChanged();
}

void PipelineStore::redo() {
    if (m_redoStack.empty())
        return;
    m_undoStack.push_back({stages, eqPresets, convPresets});
    auto snap = m_redoStack.back();
    m_redoStack.pop_back();
    stages = snap.stages;
    eqPresets = snap.eqPresets;
    convPresets = snap.convPresets;
    save();
    emit pipelineChanged();
}

void PipelineStore::clearUndoStack() {
    m_undoStack.clear();
    m_redoStack.clear();
}

// MARK: - JSON File Import / Export

QJsonObject PipelineStore::toJson() const {
    QJsonObject obj;
    QJsonArray stagesArr;
    for (const auto& st : stages)
        stagesArr.append(st.toJson());
    obj["pipelineStages"] = stagesArr;

    QJsonArray eqArr;
    for (const auto& eq : eqPresets)
        eqArr.append(eq.toJson());
    obj["eqPresets"] = eqArr;

    QJsonArray convArr;
    for (const auto& conv : convPresets)
        convArr.append(conv.toJson());
    obj["convPresets"] = convArr;

    return obj;
}

void PipelineStore::restoreFromJson(const QJsonObject& json) {
    if (json.contains("pipelineStages")) {
        stages.clear();
        for (const auto& item : json["pipelineStages"].toArray()) {
            stages.push_back(PipelineStage::fromJson(item.toObject()));
        }
    } else if (json.contains("stages")) {
        stages.clear();
        for (const auto& item : json["stages"].toArray()) {
            stages.push_back(PipelineStage::fromJson(item.toObject()));
        }
    }
    if (stages.empty()) {
        stages = PipelineStage::defaultStages();
    }

    if (json.contains("eqPresets")) {
        eqPresets.clear();
        for (const auto& item : json["eqPresets"].toArray()) {
            eqPresets.push_back(EQPreset::fromJson(item.toObject()));
        }
    }

    if (json.contains("convPresets")) {
        convPresets.clear();
        for (const auto& item : json["convPresets"].toArray()) {
            convPresets.push_back(ConvolutionPreset::fromJson(item.toObject()));
        }
    }
}

QByteArray PipelineStore::exportJson() const {
    return QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
}

bool PipelineStore::exportToJsonFile(const QString& filePath) const {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(exportJson());
    file.close();
    return true;
}

bool PipelineStore::importJson(const QByteArray& jsonData) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(jsonData, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    pushUndoSnapshot();
    restoreFromJson(doc.object());
    save();
    emit pipelineChanged();
    return true;
}

bool PipelineStore::importFromJsonFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    return importJson(data);
}

StageBuildResult PipelineStore::buildPipeline(int sampleRate, int channelCount) const {
    StageBuildResult totalResult;

    std::map<QUuid, EQPreset> eqMap;
    for (const auto& p : eqPresets)
        eqMap[p.id] = p;

    std::map<QUuid, ConvolutionPreset> convMap;
    for (const auto& p : convPresets)
        convMap[p.id] = p;

    int currentChannels = channelCount;
    for (const auto& stage : stages) {
        auto res = StageBuilders::buildStage(stage, sampleRate, currentChannels, eqMap, convMap);

        for (const auto& [k, v] : res.filters)
            totalResult.filters[k] = v;
        for (const auto& [k, v] : res.mixers)
            totalResult.mixers[k] = v;
        for (const auto& [k, v] : res.processors)
            totalResult.processors[k] = v;
        for (const auto& step : res.steps)
            totalResult.steps.push_back(step);

        if (stage.isActive() && stage.type == StageType::MatrixMixer) {
            currentChannels = stage.mixerChannelsOut;
        }
    }

    return totalResult;
}
