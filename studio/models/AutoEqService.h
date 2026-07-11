#ifndef AUTO_EQ_SERVICE_H
#define AUTO_EQ_SERVICE_H

#include "models/EQPreset.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <functional>
#include <string>
#include <vector>

struct AutoEqIndexEntry {
    std::string name;
    std::string path;
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
    void saveToDiskCache(const QByteArray& jsonBytes);
};

#endif // AUTO_EQ_SERVICE_H
