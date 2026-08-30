#ifndef LOG_TABLE_MODEL_H
#define LOG_TABLE_MODEL_H

#include "models/LogManager.h" // for LogEntry

#include <QAbstractTableModel> // for QAbstractTableModel
#include <QFont>               // for QFont
#include <QModelIndex>         // for QModelIndex
#include <QObject>             // for QObject, Q_OBJECT, slots
#include <QString>             // for QString
#include <Qt>                  // for ItemDataRole, Orientation
#include <vector>              // for vector

class LogTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit LogTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setFilterText(const QString& filterText);
    QString filterText() const { return m_filterText; }

    void refresh();
    void clear();

    const std::vector<LogEntry>& entries() const { return m_filteredEntries; }
    QString copyAllFormatted() const;
    QString copySelectedFormatted(const std::vector<int>& rows) const;

public slots:
    void onLogAppended(const LogEntry& entry);

private:
    void refilter();

    std::vector<LogEntry> m_filteredEntries;
    QString m_filterText;
    QFont m_captionFont;
    QFont m_bodyFont;
};

#endif // LOG_TABLE_MODEL_H
