#ifndef AUTO_EQ_SERVICE_H
#define AUTO_EQ_SERVICE_H

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

struct AutoEqIndexEntry {
    std::string id;
    std::string name;
    std::string path;

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
        return obj;
    }

    static AutoEqIndexEntry fromJson(const QJsonObject& json) {
        AutoEqIndexEntry entry;
        entry.id = json["id"].toString().toStdString();
        entry.name = json["name"].toString().toStdString();
        entry.path = json["path"].toString().toStdString();
        return entry;
    }
};

class AutoEqService : public QObject {
    Q_OBJECT

public:
    explicit AutoEqService(QObject* parent = nullptr);

    void fetchIndex(std::function<void(bool success, const std::vector<AutoEqIndexEntry>& entries)> callback,
                    bool forceRefresh = false);
    void fetchPreset(const AutoEqIndexEntry& entry,
                     std::function<void(bool success, std::optional<EQPreset> preset)> callback);

private:
    QNetworkAccessManager m_networkManager;
    std::vector<AutoEqIndexEntry> m_allEntries;
    bool m_isLoaded = false;

    bool loadFromDiskCache(std::vector<AutoEqIndexEntry>& entries);
    void saveToDiskCache(const std::vector<AutoEqIndexEntry>& entries);
};

#endif // AUTO_EQ_SERVICE_H
