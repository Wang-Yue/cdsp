#include "models/LogManager.h"

#include "engine/CDSPEngine.h"

#include <QSettings>

QString logLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Off:
        return "Off";
    case LogLevel::Error:
        return "Error";
    case LogLevel::Warn:
        return "Warn";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Trace:
        return "Trace";
    }
    return "Info";
}

LogLevel stringToLogLevel(const QString& str) {
    QString u = str.toUpper();
    if (u == "OFF")
        return LogLevel::Off;
    if (u == "ERROR")
        return LogLevel::Error;
    if (u == "WARN")
        return LogLevel::Warn;
    if (u == "DEBUG")
        return LogLevel::Debug;
    if (u == "TRACE")
        return LogLevel::Trace;
    return LogLevel::Info;
}

LogManager::LogManager(QObject* parent) : QObject(parent) {
    qRegisterMetaType<LogEntry>("LogEntry");
    QSettings settings;
    if (settings.contains("selectedLogLevel")) {
        m_logLevel = stringToLogLevel(settings.value("selectedLogLevel").toString());
    }
}

LogManager* LogManager::instance() {
    static LogManager instance;
    return &instance;
}

void LogManager::setEngine(CDSPEngine* engine) {
    m_engine = engine;
    if (m_engine) {
        m_engine->setLogLevel(logLevelToString(m_logLevel).toLower().toStdString());
    }
}

void LogManager::setLogLevel(LogLevel level) {
    m_logLevel = level;
    QSettings settings;
    settings.setValue("selectedLogLevel", logLevelToString(m_logLevel));
    if (m_engine) {
        m_engine->setLogLevel(logLevelToString(m_logLevel).toLower().toStdString());
    }
}

void LogManager::appendLog(LogLevel level, const QString& message) {
    LogEntry entry{QDateTime::currentDateTime(), level, message};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(entry);
        if (m_entries.size() > m_maxEntries) {
            m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - m_maxEntries));
        }
    }
    emit logAppended(entry);
}

void LogManager::appendLog(const QString& message) {
    appendLog(LogLevel::Info, message);
}

std::vector<LogEntry> LogManager::logs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

void LogManager::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
    emit logsCleared();
}
