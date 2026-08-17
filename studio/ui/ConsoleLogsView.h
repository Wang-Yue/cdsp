#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QWidget>

class ConsoleLogsView : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleLogsView(QWidget* parent = nullptr);

private slots:
    void refreshLogs();
    void onLogAppended(const LogEntry& entry);
    void copyAllLogs();

private:
    QWidget* m_headerWidget = nullptr;
    QTableWidget* m_table = nullptr;
    QComboBox* m_levelFilterCombo = nullptr;
    QCheckBox* m_autoScrollCheck = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;

    void setupUi();
};

#endif // CONSOLE_LOGS_VIEW_H
