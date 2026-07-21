#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <mutex>
#include <vector>

#include <thread>

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
    static LogManager* instance();

    void setEngine(CDSPEngine* engine);
    void setLogLevel(LogLevel level);
    LogLevel logLevel() const { return m_logLevel; }

    void appendLog(LogLevel level, const QString& message);
    void appendLog(const QString& message);
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

    void setupCapture();
    void readPipeLoop(int readFd, LogLevel defaultLevel);
    void processCapturedLine(const std::string& line, LogLevel defaultLevel);

    int m_stdoutPipe[2] = {-1, -1};
    int m_stderrPipe[2] = {-1, -1};
    std::thread m_stdoutThread;
    std::thread m_stderrThread;
    bool m_capturing = false;
};

Q_DECLARE_METATYPE(LogEntry)

#endif // LOG_MANAGER_H
