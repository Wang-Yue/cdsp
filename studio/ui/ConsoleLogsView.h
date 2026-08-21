#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogTableModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QWidget>

class ConsoleLogsView : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleLogsView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void copyAllLogs();

private:
    QLabel* m_logCountLabel = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_levelFilterCombo = nullptr;
    QCheckBox* m_autoScrollCheck = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QTableView* m_table = nullptr;
    LogTableModel* m_model = nullptr;

    void setupUi();
    void updateCountLabel();
};

#endif // CONSOLE_LOGS_VIEW_H
