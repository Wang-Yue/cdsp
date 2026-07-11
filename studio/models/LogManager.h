#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <mutex>
#include <vector>

enum class LogLevel { Error, Warn, Info, Debug, Trace };

QString logLevelToString(LogLevel level);
LogLevel stringToLogLevel(const QString& str);

struct LogEntry {
    QDateTime timestamp;
    LogLevel level;
    QString message;
};

class LogManager : public QObject {
    Q_OBJECT

public:
    explicit LogManager(QObject* parent = nullptr);
    static LogManager* instance();

    void appendLog(LogLevel level, const QString& message);
    std::vector<LogEntry> logs(LogLevel minLevel = LogLevel::Trace, const QString& searchFilter = "") const;
    void clear();

signals:
    void logAppended(const LogEntry& entry);
    void logsCleared();

private:
    mutable std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    size_t m_maxEntries = 2000;
};

Q_DECLARE_METATYPE(LogEntry)

#endif // LOG_MANAGER_H
