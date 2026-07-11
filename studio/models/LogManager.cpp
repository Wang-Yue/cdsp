#include "models/LogManager.h"

QString logLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Trace: return "TRACE";
    }
    return "INFO";
}

LogLevel stringToLogLevel(const QString& str) {
    QString u = str.toUpper();
    if (u == "ERROR") return LogLevel::Error;
    if (u == "WARN") return LogLevel::Warn;
    if (u == "DEBUG") return LogLevel::Debug;
    if (u == "TRACE") return LogLevel::Trace;
    return LogLevel::Info;
}

LogManager::LogManager(QObject* parent) : QObject(parent) {}

LogManager* LogManager::instance() {
    static LogManager instance;
    return &instance;
}

void LogManager::appendLog(LogLevel level, const QString& message) {
    LogEntry entry{ QDateTime::currentDateTime(), level, message };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(entry);
        if (m_entries.size() > m_maxEntries) {
            m_entries.erase(m_entries.begin());
        }
    }
    emit logAppended(entry);
}

std::vector<LogEntry> LogManager::logs(LogLevel minLevel, const QString& searchFilter) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<LogEntry> res;
    for (const auto& e : m_entries) {
        if (static_cast<int>(e.level) <= static_cast<int>(minLevel)) {
            if (searchFilter.isEmpty() || e.message.contains(searchFilter, Qt::CaseInsensitive)) {
                res.push_back(e);
            }
        }
    }
    return res;
}

void LogManager::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
    emit logsCleared();
}
