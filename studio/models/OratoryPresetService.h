#ifndef ORATORY_PRESET_SERVICE_H
#define ORATORY_PRESET_SERVICE_H

#include "models/EQPreset.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <vector>
#include <string>
#include <functional>

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

    void fetchIndex(std::function<void(bool success, const std::vector<OratoryIndexEntry>& entries)> callback);
    void fetchPreset(const OratoryIndexEntry& entry, std::function<void(bool success, std::optional<EQPreset> preset)> callback);

private:
    QNetworkAccessManager m_networkManager;
};

#endif // ORATORY_PRESET_SERVICE_H
