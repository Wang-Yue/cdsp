#include "models/AutoEqService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

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
    if (!doc.isObject())
        return false;

    QJsonObject root = doc.object();
    QJsonArray tree = root["tree"].toArray();
    entries.clear();

    for (const auto& item : tree) {
        QJsonObject obj = item.toObject();
        QString path = obj["path"].toString();

        if (path.startsWith("results/") && path.endsWith(" ParametricEQ.txt")) {
            QString fileName = path.section('/', -1);
            QString headphoneName = fileName;
            headphoneName.replace(" ParametricEQ.txt", "");

            AutoEqIndexEntry entry;
            entry.name = headphoneName.toStdString();
            entry.path = path.toStdString();
            entries.push_back(entry);
        }
    }
    return !entries.empty();
}

void AutoEqService::saveToDiskCache(const QByteArray& jsonBytes) {
    QFile file(getAutoEqCacheFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(jsonBytes);
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
        weakThis->saveToDiskCache(data);

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
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entries.push_back(entry);
            }
        }
        weakThis->m_allEntries = entries;
        weakThis->m_isLoaded = true;
        callback(true, weakThis->m_allEntries);
    });
}

void AutoEqService::fetchPreset(const AutoEqIndexEntry& entry,
                                std::function<void(bool success, std::optional<EQPreset> preset)> callback) {
    QString downloadPath = QString::fromStdString(entry.path).replace(" ", "%20");
    QString rawUrlStr = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/" + downloadPath;

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
