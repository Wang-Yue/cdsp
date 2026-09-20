#ifndef CONSOLE_LOGS_VIEW_H
#define CONSOLE_LOGS_VIEW_H

#include "models/LogTableModel.h" // for LogTableModel

#include <QCheckBox>   // for QCheckBox
#include <QComboBox>   // for QComboBox
#include <QLabel>      // for QLabel
#include <QLineEdit>   // for QLineEdit
#include <QObject>     // for Q_OBJECT, slots
#include <QPushButton> // for QPushButton
#include <QTableView>  // for QTableView
#include <QWidget>     // for QWidget

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
    void updateRowHeight(int row);
    void updateAllRowHeights();
    int m_lastColWidth = 0;
};

#endif // CONSOLE_LOGS_VIEW_H
