#ifndef AUTO_EQ_PICKER_DLG_H
#define AUTO_EQ_PICKER_DLG_H

#include "models/AutoEqService.h"
#include "models/PipelineStore.h"
#include "models/DSPEngineController.h"
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <memory>

class AutoEqPickerDlg : public QDialog {
    Q_OBJECT

public:
public:
    AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr)
        : AutoEqPickerDlg(pipeline, nullptr, parent) {}
    AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline, std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void onImportClicked();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;
    AutoEqService m_service;
    std::vector<AutoEqIndexEntry> m_entries;

    QLineEdit* m_searchEdit;
    QListWidget* m_listWidget;
    QPushButton* m_importBtn;
    QLabel* m_statusLabel;

    void setupUi();
    void loadIndex(bool forceRefresh = false);
};

#endif // AUTO_EQ_PICKER_DLG_H
