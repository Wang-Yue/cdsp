#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
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
    QTableWidget* m_table;
    QComboBox* m_levelFilterCombo;
    QCheckBox* m_autoScrollCheck;
    QPushButton* m_copyBtn;
    QPushButton* m_clearBtn;

    void setupUi();
};

#endif // CONSOLE_LOGS_VIEW_H
