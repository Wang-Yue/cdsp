#ifndef ORATORY_PRESET_SERVICE_H
#define ORATORY_PRESET_SERVICE_H

#include "models/EQPreset.h" // for EQPreset

#include <QJsonObject>           // for QJsonObject
#include <QJsonValue>            // for QJsonValue, QJsonValueRef
#include <QNetworkAccessManager> // for QNetworkAccessManager
#include <QObject>               // for QObject, Q_OBJECT
#include <QString>               // for QString
#include <functional>            // for function
#include <optional>              // for optional
#include <string>                // for basic_string, string
#include <vector>                // for vector

struct OratoryIndexEntry {
    std::string id;
    std::string name;
    std::string path;
    std::string author = "oratory1990";
    std::string url;

    std::string downloadPath() const {
        QString p = QString::fromStdString(path);
        p.replace(" ", "%20");
        return p.toStdString();
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = QString::fromStdString(id);
        obj["name"] = QString::fromStdString(name);
        obj["path"] = QString::fromStdString(path);
        obj["author"] = QString::fromStdString(author);
        obj["url"] = QString::fromStdString(url);
        return obj;
    }

    static OratoryIndexEntry fromJson(const QJsonObject& json) {
        OratoryIndexEntry entry;
        entry.id = json["id"].toString().toStdString();
        entry.name = json["name"].toString().toStdString();
        entry.path = json["path"].toString().toStdString();
        entry.author = json["author"].toString("oratory1990").toStdString();
        entry.url = json["url"].toString().toStdString();
        return entry;
    }
};

class OratoryPresetService : public QObject {
    Q_OBJECT

public:
    explicit OratoryPresetService(QObject* parent = nullptr);

    void fetchIndex(std::function<void(bool success, const std::vector<OratoryIndexEntry>& entries)> callback,
                    bool forceRefresh = false);
    void fetchPreset(const OratoryIndexEntry& entry,
                     std::function<void(bool success, std::optional<EQPreset> preset)> callback);

private:
    QNetworkAccessManager m_networkManager;
    std::vector<OratoryIndexEntry> m_allEntries;
    bool m_isLoaded = false;

    bool loadFromDiskCache(std::vector<OratoryIndexEntry>& entries);
    void saveToDiskCache(const std::vector<OratoryIndexEntry>& entries);
};

#endif // ORATORY_PRESET_SERVICE_H
