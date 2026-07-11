#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogManager.h"
#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>

class ConsoleLogsView : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleLogsView(QWidget* parent = nullptr);

private slots:
    void refreshLogs();
    void onLogAppended(const LogEntry& entry);
    void copySelectedLogs();
    void copyAllLogs();

private:
    QTableWidget* m_table;
    QLineEdit* m_searchEdit;
    QComboBox* m_levelFilterCombo;
    QPushButton* m_copyBtn;
    QPushButton* m_clearBtn;
    QLabel* m_logCountLabel;

    void setupUi();
};

#endif // CONSOLE_LOGS_VIEW_H
