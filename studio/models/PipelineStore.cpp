#include "models/PipelineStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>

PipelineStore::PipelineStore(QObject* parent) : QObject(parent) {
    load();
    ensureDefaultPresets();
}

void PipelineStore::ensureDefaultPresets() {
    if (eqPresets.empty()) {
        EQBand b1(EQBandType::Peaking, 100.0, 3.0, 1.41);
        EQBand b2(EQBandType::Peaking, 1000.0, -2.0, 2.0);
        EQBand b3(EQBandType::Highshelf, 5000.0, 1.5, 0.707);
        eqPresets.push_back(EQPreset("Default EQ", -3.0, {b1, b2, b3}));
    }
    if (stages.empty()) {
        stages.push_back(PipelineStage(StageType::Volume, "Master Volume"));
    }
}

QUuid PipelineStore::addStage(StageType type) {
    PipelineStage stage(type);
    if (type == StageType::EQ && !eqPresets.empty()) {
        stage.eqPresetId = eqPresets[0].id;
    }
    if (type == StageType::Convolution && !convPresets.empty()) {
        stage.convPresetId = convPresets[0].id;
    }
    stages.push_back(stage);
    save();
    emit pipelineChanged();
    return stage.id;
}

QUuid PipelineStore::duplicateStage(const QUuid& id) {
    auto it = std::find_if(stages.begin(), stages.end(), [&id](const PipelineStage& s) { return s.id == id; });
    if (it != stages.end()) {
        PipelineStage dup = *it;
        dup.id = QUuid::createUuid();
        dup.name = dup.name + " (Copy)";
        stages.insert(it + 1, dup);
        save();
        emit pipelineChanged();
        return dup.id;
    }
    return QUuid();
}

int PipelineStore::channelCountBeforeStage(size_t index, int captureChannels) const {
    int current = captureChannels;
    for (size_t i = 0; i < index && i < stages.size(); ++i) {
        const auto& stage = stages[i];
        if (stage.isEnabled && stage.type == StageType::MatrixMixer) {
            current = stage.mixerChannelsOut;
        }
    }
    return current;
}

void PipelineStore::deleteStage(const QUuid& id) {
    stages.erase(std::remove_if(stages.begin(), stages.end(), [&id](const PipelineStage& s) { return s.id == id; }),
                 stages.end());
    save();
    emit pipelineChanged();
}

void PipelineStore::moveStage(int from, int to) {
    if (from < 0 || from >= static_cast<int>(stages.size()))
        return;
    if (to < 0 || to >= static_cast<int>(stages.size()))
        return;
    PipelineStage stage = stages[from];
    stages.erase(stages.begin() + from);
    stages.insert(stages.begin() + to, stage);
    save();
    emit pipelineChanged();
}

QUuid PipelineStore::addEQPreset(const EQPreset& preset) {
    eqPresets.push_back(preset);
    save();
    emit pipelineChanged();
    return preset.id;
}

void PipelineStore::updateEQPreset(const EQPreset& preset) {
    for (auto& p : eqPresets) {
        if (p.id == preset.id) {
            p = preset;
            break;
        }
    }
    save();
    emit pipelineChanged();
}

void PipelineStore::deleteEQPreset(const QUuid& id) {
    eqPresets.erase(std::remove_if(eqPresets.begin(), eqPresets.end(), [&id](const EQPreset& p) { return p.id == id; }),
                    eqPresets.end());
    save();
    emit pipelineChanged();
}

QUuid PipelineStore::addConvPreset(const ConvolutionPreset& preset) {
    convPresets.push_back(preset);
    save();
    emit pipelineChanged();
    return preset.id;
}

void PipelineStore::updateConvPreset(const ConvolutionPreset& preset) {
    for (auto& p : convPresets) {
        if (p.id == preset.id) {
            p = preset;
            break;
        }
    }
    save();
    emit pipelineChanged();
}

void PipelineStore::deleteConvPreset(const QUuid& id) {
    for (const auto& preset : convPresets) {
        if (preset.id == id) {
            for (const auto& [rate, path] : preset.irPaths) {
                if (!path.empty()) {
                    QFile::remove(QString::fromStdString(path));
                }
            }
            break;
        }
    }
    convPresets.erase(std::remove_if(convPresets.begin(), convPresets.end(),
                                     [&id](const ConvolutionPreset& p) { return p.id == id; }),
                      convPresets.end());
    save();
    emit pipelineChanged();
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

void PipelineStore::load() {
    QSettings s("DSPMonitor", "MonitorQt");

    if (s.contains("stages")) {
        stages.clear();
        QJsonArray arr = QJsonDocument::fromJson(s.value("stages").toByteArray()).array();
        for (const auto& item : arr)
            stages.push_back(PipelineStage::fromJson(item.toObject()));
    }
    if (s.contains("eqPresets")) {
        eqPresets.clear();
        QJsonArray arr = QJsonDocument::fromJson(s.value("eqPresets").toByteArray()).array();
        for (const auto& item : arr)
            eqPresets.push_back(EQPreset::fromJson(item.toObject()));
    }
    if (s.contains("convPresets")) {
        convPresets.clear();
        QJsonArray arr = QJsonDocument::fromJson(s.value("convPresets").toByteArray()).array();
        for (const auto& item : arr)
            convPresets.push_back(ConvolutionPreset::fromJson(item.toObject()));
    }
}

void PipelineStore::save() {
    QSettings s("DSPMonitor", "MonitorQt");

    QJsonArray stagesArr;
    for (const auto& st : stages)
        stagesArr.append(st.toJson());
    s.setValue("stages", QJsonDocument(stagesArr).toJson(QJsonDocument::Compact));

    QJsonArray eqArr;
    for (const auto& eq : eqPresets)
        eqArr.append(eq.toJson());
    s.setValue("eqPresets", QJsonDocument(eqArr).toJson(QJsonDocument::Compact));

    QJsonArray convArr;
    for (const auto& conv : convPresets)
        convArr.append(conv.toJson());
    s.setValue("convPresets", QJsonDocument(convArr).toJson(QJsonDocument::Compact));
}
