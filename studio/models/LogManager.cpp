#include "models/LogManager.h"

#include "engine/CDSPEngine.h"

#include <QSettings>
#include <cstdio>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

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
    setupCapture();
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

void LogManager::setupCapture() {
#ifndef _WIN32
    // Disable buffering for stdout and stderr so we get real-time output
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    // Create stdout pipe
    if (pipe(m_stdoutPipe) == 0) {
        if (dup2(m_stdoutPipe[1], STDOUT_FILENO) != -1) {
            close(m_stdoutPipe[1]);
            m_stdoutThread = std::thread(&LogManager::readPipeLoop, this, m_stdoutPipe[0], LogLevel::Info);
            m_stdoutThread.detach();
            m_capturing = true;
        } else {
            close(m_stdoutPipe[0]);
            close(m_stdoutPipe[1]);
        }
    }

    // Create stderr pipe
    if (pipe(m_stderrPipe) == 0) {
        if (dup2(m_stderrPipe[1], STDERR_FILENO) != -1) {
            close(m_stderrPipe[1]);
            m_stderrThread = std::thread(&LogManager::readPipeLoop, this, m_stderrPipe[0], LogLevel::Error);
            m_stderrThread.detach();
            m_capturing = true;
        } else {
            close(m_stderrPipe[0]);
            close(m_stderrPipe[1]);
        }
    }
#endif
}

void LogManager::readPipeLoop(int readFd, LogLevel defaultLevel) {
#ifndef _WIN32
    char buffer[4096];
    std::string accumulator;
    while (true) {
        ssize_t bytesRead = read(readFd, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0) {
            break; // EOF or error
        }
        buffer[bytesRead] = '\0';
        accumulator += buffer;

        size_t pos;
        while ((pos = accumulator.find('\n')) != std::string::npos) {
            std::string line = accumulator.substr(0, pos);
            accumulator.erase(0, pos + 1);
            processCapturedLine(line, defaultLevel);
        }
    }
    close(readFd);
#else
    (void)readFd;
    (void)defaultLevel;
#endif
}

void LogManager::processCapturedLine(const std::string& line, LogLevel defaultLevel) {
    if (line.empty()) return;

    QString qline = QString::fromStdString(line).trimmed();
    LogLevel level = defaultLevel;

    // Parse level prefix if present, e.g. "[INFO] Component: Message"
    if (qline.startsWith("[")) {
        int closeBracket = qline.indexOf(']');
        if (closeBracket > 0) {
            QString levelStr = qline.mid(1, closeBracket - 1).trimmed().toUpper();
            bool hasLevel = true;
            if (levelStr == "ERROR") {
                level = LogLevel::Error;
            } else if (levelStr == "WARN" || levelStr == "WARNING") {
                level = LogLevel::Warn;
            } else if (levelStr == "INFO") {
                level = LogLevel::Info;
            } else if (levelStr == "DEBUG") {
                level = LogLevel::Debug;
            } else if (levelStr == "TRACE") {
                level = LogLevel::Trace;
            } else {
                hasLevel = false;
            }
            if (hasLevel) {
                qline = qline.mid(closeBracket + 1).trimmed();
            }
        }
    }

    appendLog(level, qline);
}
