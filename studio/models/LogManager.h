#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <QDateTime> // for QDateTime
#include <QObject>   // for QObject, Q_OBJECT, signals
#include <QString>   // for QString
#include <mutex>     // for mutex
#include <stddef.h>  // for size_t
#include <vector>    // for vector

enum class LogLevel { Off, Error, Warn, Info, Debug, Trace };

QString logLevelToString(LogLevel level);
LogLevel stringToLogLevel(const QString& str);

struct LogEntry {
    QDateTime timestamp;
    LogLevel level = LogLevel::Info;
    QString message;
};

class CDSPEngine;

class LogManager : public QObject {
    Q_OBJECT

public:
    explicit LogManager(QObject* parent = nullptr);
    ~LogManager() override;
    static LogManager* instance();

    void setEngine(CDSPEngine* engine);
    void setLogLevel(LogLevel level);
    LogLevel logLevel() const { return m_logLevel; }

    void appendLog(LogLevel level, const QString& message);
    void appendLog(const QString& message);
    void onCdspLog(LogLevel level, const QString& component, const QString& message);

    std::vector<LogEntry> logs() const;
    void clear();

signals:
    void logAppended(const LogEntry& entry);
    void logsCleared();

private:
    mutable std::mutex m_mutex;
    CDSPEngine* m_engine = nullptr;
    LogLevel m_logLevel = LogLevel::Info;
    std::vector<LogEntry> m_entries;
    size_t m_maxEntries = 2000;
};

namespace AppLogger {
void log(LogLevel level, const QString& component, const QString& message);
void info(const QString& component, const QString& message);
void warn(const QString& component, const QString& message);
void error(const QString& component, const QString& message);
void debug(const QString& component, const QString& message);
void trace(const QString& component, const QString& message);
} // namespace AppLogger

#endif // LOG_MANAGER_H
