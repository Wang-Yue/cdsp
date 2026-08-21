#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>

class ConsoleLogsView : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleLogsView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void refreshLogs();
    void onLogAppended(const LogEntry& entry);
    void copyAllLogs();

private:
    QLabel* m_logCountLabel = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_levelFilterCombo = nullptr;
    QCheckBox* m_autoScrollCheck = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QTableWidget* m_table = nullptr;

    void setupUi();
};

#endif // CONSOLE_LOGS_VIEW_H
