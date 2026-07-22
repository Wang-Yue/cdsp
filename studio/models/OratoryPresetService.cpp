#include "models/OratoryPresetService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

OratoryPresetService::OratoryPresetService(QObject* parent) : QObject(parent) {}

static QString getOratoryCacheFilePath() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    return cacheDir + "/oratory_index.json";
}

bool OratoryPresetService::loadFromDiskCache(std::vector<OratoryIndexEntry>& entries) {
    QFile file(getOratoryCacheFilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    entries.clear();

    if (doc.isArray()) {
        QJsonArray arr = doc.array();
        for (const auto& item : arr) {
            entries.push_back(OratoryIndexEntry::fromJson(item.toObject()));
        }
        return !entries.empty();
    }

    if (doc.isObject()) {
        QJsonObject root = doc.object();
        QJsonArray tree = root["tree"].toArray();

        for (const auto& item : tree) {
            QJsonObject obj = item.toObject();
            QString path = obj["path"].toString();

            if (path.endsWith(" ParametricEQ.txt")) {
                QString fileName = path.section('/', -1);
                QString headphoneName = fileName;
                headphoneName.replace(" ParametricEQ.txt", "");

                OratoryIndexEntry entry;
                entry.id = obj["sha"].toString().toStdString();
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entry.author = "oratory1990";
                entry.url = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/oratory1990/" +
                            entry.downloadPath();
                entries.push_back(entry);
            }
        }
        return !entries.empty();
    }
    return false;
}

void OratoryPresetService::saveToDiskCache(const std::vector<OratoryIndexEntry>& entries) {
    QJsonArray arr;
    for (const auto& entry : entries) {
        arr.append(entry.toJson());
    }
    QJsonDocument doc(arr);
    QFile file(getOratoryCacheFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Compact));
        file.close();
    }
}

void OratoryPresetService::fetchIndex(
    std::function<void(bool success, const std::vector<OratoryIndexEntry>& entries)> callback, bool forceRefresh) {
    if (!forceRefresh && m_isLoaded) {
        callback(true, m_allEntries);
        return;
    }

    if (!forceRefresh && loadFromDiskCache(m_allEntries)) {
        m_isLoaded = true;
        callback(true, m_allEntries);
        return;
    }

    QUrl url("https://api.github.com/repos/jaakkopasanen/AutoEq/git/trees/master:results/oratory1990?recursive=1");
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "DSPMonitor");

    QNetworkReply* reply = m_networkManager.get(request);
    QPointer<OratoryPresetService> weakThis(this);
    connect(reply, &QNetworkReply::finished, [weakThis, reply, callback]() {
        reply->deleteLater();
        if (!weakThis)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            if (weakThis->loadFromDiskCache(weakThis->m_allEntries)) {
                weakThis->m_isLoaded = true;
                callback(true, weakThis->m_allEntries);
            } else {
                callback(false, {});
            }
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            if (weakThis->loadFromDiskCache(weakThis->m_allEntries)) {
                weakThis->m_isLoaded = true;
                callback(true, weakThis->m_allEntries);
            } else {
                callback(false, {});
            }
            return;
        }

        QJsonObject root = doc.object();
        QJsonArray tree = root["tree"].toArray();
        std::vector<OratoryIndexEntry> entries;

        for (const auto& item : tree) {
            QJsonObject obj = item.toObject();
            QString path = obj["path"].toString();

            if (path.endsWith(" ParametricEQ.txt")) {
                QString fileName = path.section('/', -1);
                QString headphoneName = fileName;
                headphoneName.replace(" ParametricEQ.txt", "");

                OratoryIndexEntry entry;
                entry.id = obj["sha"].toString().toStdString();
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entry.author = "oratory1990";
                entry.url = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/oratory1990/" +
                            entry.downloadPath();
                entries.push_back(entry);
            }
        }
        weakThis->m_allEntries = entries;
        weakThis->m_isLoaded = true;
        weakThis->saveToDiskCache(weakThis->m_allEntries);
        callback(true, weakThis->m_allEntries);
    });
}

void OratoryPresetService::fetchPreset(const OratoryIndexEntry& entry,
                                       std::function<void(bool success, std::optional<EQPreset> preset)> callback) {
    QString rawUrlStr = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/oratory1990/" +
                        QString::fromStdString(entry.downloadPath());

    QUrl url(rawUrlStr);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "DSPMonitor");

    QNetworkReply* reply = m_networkManager.get(request);
    connect(reply, &QNetworkReply::finished, [reply, entry, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, std::nullopt);
            return;
        }

        std::string csvText = reply->readAll().toStdString();
        auto presetOpt = EQPreset::fromCSV(csvText, entry.name);
        callback(presetOpt.has_value(), presetOpt);
    });
}
