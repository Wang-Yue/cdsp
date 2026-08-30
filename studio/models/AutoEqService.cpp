#include "models/AutoEqService.h"

#include <QByteArray>      // for QByteArray
#include <QChar>           // for QChar
#include <QDir>            // for QDir
#include <QFile>           // for QFile
#include <QIODevice>       // for QIODevice
#include <QJsonArray>      // for QJsonArray
#include <QJsonDocument>   // for QJsonDocument
#include <QJsonObject>     // for QJsonObject
#include <QNetworkReply>   // for QNetworkReply
#include <QNetworkRequest> // for QNetworkRequest
#include <QPointer>        // for QPointer
#include <QStandardPaths>  // for QStandardPaths
#include <QUrl>            // for QUrl

AutoEqService::AutoEqService(QObject* parent) : QObject(parent) {}

static QString getAutoEqCacheFilePath() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    return cacheDir + "/autoeq_index.json";
}

bool AutoEqService::loadFromDiskCache(std::vector<AutoEqIndexEntry>& entries) {
    QFile file(getAutoEqCacheFilePath());
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
            entries.push_back(AutoEqIndexEntry::fromJson(item.toObject()));
        }
        return !entries.empty();
    }

    if (doc.isObject()) {
        QJsonObject root = doc.object();
        QJsonArray tree = root["tree"].toArray();

        for (const auto& item : tree) {
            QJsonObject obj = item.toObject();
            QString path = obj["path"].toString();

            if (path.startsWith("results/") && path.endsWith(" ParametricEQ.txt")) {
                QString fileName = path.section('/', -1);
                QString headphoneName = fileName;
                headphoneName.replace(" ParametricEQ.txt", "");

                AutoEqIndexEntry entry;
                entry.id = obj["sha"].toString().toStdString();
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entries.push_back(entry);
            }
        }
        return !entries.empty();
    }
    return false;
}

void AutoEqService::saveToDiskCache(const std::vector<AutoEqIndexEntry>& entries) {
    QJsonArray arr;
    for (const auto& entry : entries) {
        arr.append(entry.toJson());
    }
    QJsonDocument doc(arr);
    QFile file(getAutoEqCacheFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Compact));
        file.close();
    }
}

void AutoEqService::fetchIndex(std::function<void(bool success, const std::vector<AutoEqIndexEntry>& entries)> callback,
                               bool forceRefresh) {
    if (!forceRefresh && m_isLoaded) {
        callback(true, m_allEntries);
        return;
    }

    if (!forceRefresh && loadFromDiskCache(m_allEntries)) {
        m_isLoaded = true;
        callback(true, m_allEntries);
        return;
    }

    QUrl url("https://api.github.com/repos/jaakkopasanen/AutoEq/git/trees/master?recursive=1");
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "DSPMonitor");

    QNetworkReply* reply = m_networkManager.get(request);
    QPointer<AutoEqService> weakThis(this);
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
        std::vector<AutoEqIndexEntry> entries;

        for (const auto& item : tree) {
            QJsonObject obj = item.toObject();
            QString path = obj["path"].toString();

            if (path.startsWith("results/") && path.endsWith(" ParametricEQ.txt")) {
                QString fileName = path.section('/', -1);
                QString headphoneName = fileName;
                headphoneName.replace(" ParametricEQ.txt", "");

                AutoEqIndexEntry entry;
                entry.id = obj["sha"].toString().toStdString();
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entries.push_back(entry);
            }
        }
        weakThis->m_allEntries = entries;
        weakThis->m_isLoaded = true;
        weakThis->saveToDiskCache(weakThis->m_allEntries);
        callback(true, weakThis->m_allEntries);
    });
}

void AutoEqService::fetchPreset(const AutoEqIndexEntry& entry,
                                std::function<void(bool success, std::optional<EQPreset> preset)> callback) {
    QString rawUrlStr =
        "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/" + QString::fromStdString(entry.downloadPath());

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
