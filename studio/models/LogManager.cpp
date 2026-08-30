#include "models/LogManager.h"

#include "engine/CDSPEngine.h" // for CDSPEngine

#include <QMetaObject> // for QMetaObject
#include <QMetaType>   // for qRegisterMetaType
#include <QSettings>   // for QSettings
#include <QVariant>    // for QVariant
#include <Qt>          // for ConnectionType
#include <string>      // for string, basic_string

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
    CDSPEngine::setLogCallback([this](const std::string& level, const std::string& label, const std::string& message) {
        onCdspLog(stringToLogLevel(QString::fromStdString(level)), QString::fromStdString(label),
                  QString::fromStdString(message));
    });
}

LogManager::~LogManager() {
    CDSPEngine::setLogCallback(nullptr);
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

void LogManager::onCdspLog(LogLevel level, const QString& component, const QString& message) {
    QString fullMsg;
    if (!component.isEmpty()) {
        fullMsg = QString("[%1] %2").arg(component, message);
    } else {
        fullMsg = QString("%1").arg(message);
    }
    QMetaObject::invokeMethod(this, [this, level, fullMsg]() { appendLog(level, fullMsg); }, Qt::QueuedConnection);
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

namespace AppLogger {
void log(LogLevel level, const QString& component, const QString& message) {
    QString formatted;
    if (!component.isEmpty()) {
        formatted = QString("[%1] %2").arg(component, message);
    } else {
        formatted = QString("%1").arg(message);
    }
    if (LogManager::instance()) {
        LogManager::instance()->appendLog(level, formatted);
    }
}

void info(const QString& component, const QString& message) {
    log(LogLevel::Info, component, message);
}

void warn(const QString& component, const QString& message) {
    log(LogLevel::Warn, component, message);
}

void error(const QString& component, const QString& message) {
    log(LogLevel::Error, component, message);
}

void debug(const QString& component, const QString& message) {
    log(LogLevel::Debug, component, message);
}

void trace(const QString& component, const QString& message) {
    log(LogLevel::Trace, component, message);
}
} // namespace AppLogger
