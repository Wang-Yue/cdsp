#include "models/LogTableModel.h"

#include <QColor>        // for QColor
#include <QDateTime>     // for QDateTime
#include <QFlags>        // for QFlags
#include <QFontDatabase> // for QFontDatabase
#include <QList>         // for QList
#include <QStringList>   // for QStringList
#include <QVariant>      // for QVariant
#include <stddef.h>      // for size_t

LogTableModel::LogTableModel(QObject* parent) : QAbstractTableModel(parent) {
    m_captionFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_captionFont.setPointSize(11);

    m_bodyFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_bodyFont.setPointSize(12);

    if (LogManager::instance()) {
        connect(LogManager::instance(), &LogManager::logAppended, this, &LogTableModel::onLogAppended);
        connect(LogManager::instance(), &LogManager::logsCleared, this, &LogTableModel::clear);
    }
    refilter();
}

int LogTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_filteredEntries.size());
}

int LogTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return 3;
}

QVariant LogTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case 0:
            return tr("Timestamp");
        case 1:
            return tr("Level");
        case 2:
            return tr("Message");
        default:
            break;
        }
    }
    return {};
}

QVariant LogTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_filteredEntries.size())
        return {};

    const auto& entry = m_filteredEntries[static_cast<size_t>(index.row())];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0:
            return entry.timestamp.toString("HH:mm:ss.zzz");
        case 1:
            return logLevelToString(entry.level);
        case 2:
            return entry.message;
        default:
            break;
        }
    } else if (role == Qt::ForegroundRole) {
        if (index.column() == 0) {
            return QColor(128, 128, 128);
        } else if (index.column() == 1) {
            switch (entry.level) {
            case LogLevel::Error:
                return QColor(230, 50, 50);
            case LogLevel::Warn:
                return QColor(230, 140, 20);
            case LogLevel::Info:
                return QColor(40, 160, 80);
            case LogLevel::Debug:
                return QColor(80, 140, 220);
            default:
                return QColor(128, 128, 128);
            }
        }
    } else if (role == Qt::FontRole) {
        if (index.column() == 0 || index.column() == 1) {
            return m_captionFont;
        } else if (index.column() == 2) {
            return m_bodyFont;
        }
    } else if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);
    }

    return {};
}

void LogTableModel::setFilterText(const QString& filterText) {
    if (m_filterText == filterText)
        return;
    m_filterText = filterText;
    refilter();
}

void LogTableModel::refresh() {
    refilter();
}

void LogTableModel::clear() {
    beginResetModel();
    m_filteredEntries.clear();
    endResetModel();
}

void LogTableModel::refilter() {
    beginResetModel();
    m_filteredEntries.clear();
    if (LogManager::instance()) {
        auto allLogs = LogManager::instance()->logs();
        m_filteredEntries.reserve(allLogs.size());
        for (const auto& e : allLogs) {
            if (m_filterText.isEmpty() || e.message.contains(m_filterText, Qt::CaseInsensitive)) {
                m_filteredEntries.push_back(e);
            }
        }
    }
    endResetModel();
}

void LogTableModel::onLogAppended(const LogEntry& entry) {
    if (!m_filterText.isEmpty() && !entry.message.contains(m_filterText, Qt::CaseInsensitive)) {
        return;
    }

    constexpr size_t kMaxRows = 2000;
    if (m_filteredEntries.size() >= kMaxRows) {
        beginRemoveRows(QModelIndex(), 0, 0);
        m_filteredEntries.erase(m_filteredEntries.begin());
        endRemoveRows();
    }

    int row = static_cast<int>(m_filteredEntries.size());
    beginInsertRows(QModelIndex(), row, row);
    m_filteredEntries.push_back(entry);
    endInsertRows();
}

QString LogTableModel::copyAllFormatted() const {
    QStringList lines;
    lines.reserve(static_cast<int>(m_filteredEntries.size()));
    for (const auto& entry : m_filteredEntries) {
        lines.append(
            QString("[%1] [%2] %3")
                .arg(entry.timestamp.toString(Qt::ISODateWithMs), logLevelToString(entry.level), entry.message));
    }
    return lines.join("\n");
}

QString LogTableModel::copySelectedFormatted(const std::vector<int>& rows) const {
    QStringList lines;
    lines.reserve(static_cast<int>(rows.size()));
    for (int r : rows) {
        if (r >= 0 && static_cast<size_t>(r) < m_filteredEntries.size()) {
            const auto& entry = m_filteredEntries[static_cast<size_t>(r)];
            lines.append(
                QString("[%1] [%2] %3")
                    .arg(entry.timestamp.toString(Qt::ISODateWithMs), logLevelToString(entry.level), entry.message));
        }
    }
    return lines.join("\n");
}
