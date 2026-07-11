#ifndef ORATORY_PRESET_SERVICE_H
#define ORATORY_PRESET_SERVICE_H

#include "models/EQPreset.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <functional>
#include <string>
#include <vector>

struct OratoryIndexEntry {
    std::string name;
    std::string path;
    std::string author;
    std::string url;
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
    void saveToDiskCache(const QByteArray& jsonBytes);
};

#endif // ORATORY_PRESET_SERVICE_H
