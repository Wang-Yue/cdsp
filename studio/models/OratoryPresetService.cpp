#include "models/OratoryPresetService.h"
#include <QUrl>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

OratoryPresetService::OratoryPresetService(QObject* parent) : QObject(parent) {}

void OratoryPresetService::fetchIndex(std::function<void(bool success, const std::vector<OratoryIndexEntry>& entries)> callback) {
    QUrl url("https://api.github.com/repos/jaakkopasanen/AutoEq/git/trees/master:results/oratory1990?recursive=1");
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "DSPMonitor");

    QNetworkReply* reply = m_networkManager.get(request);
    connect(reply, &QNetworkReply::finished, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {});
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            callback(false, {});
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
                entry.name = headphoneName.toStdString();
                entry.path = path.toStdString();
                entries.push_back(entry);
            }
        }
        callback(true, entries);
    });
}

void OratoryPresetService::fetchPreset(const OratoryIndexEntry& entry, std::function<void(bool success, std::optional<EQPreset> preset)> callback) {
    QString downloadPath = QString::fromStdString(entry.path).replace(" ", "%20");
    QString rawUrlStr = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/oratory1990/" + downloadPath;

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
