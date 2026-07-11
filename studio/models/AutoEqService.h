#ifndef AUTO_EQ_SERVICE_H
#define AUTO_EQ_SERVICE_H

#include "models/EQPreset.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <vector>
#include <string>
#include <functional>

struct AutoEqIndexEntry {
    std::string name;
    std::string path;
};

class AutoEqService : public QObject {
    Q_OBJECT

public:
    explicit AutoEqService(QObject* parent = nullptr);

    void fetchIndex(std::function<void(bool success, const std::vector<AutoEqIndexEntry>& entries)> callback);
    void fetchPreset(const AutoEqIndexEntry& entry, std::function<void(bool success, std::optional<EQPreset> preset)> callback);

private:
    QNetworkAccessManager m_networkManager;
};

#endif // AUTO_EQ_SERVICE_H
