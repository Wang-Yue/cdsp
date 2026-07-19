#ifndef AUTO_EQ_PICKER_DLG_H
#define AUTO_EQ_PICKER_DLG_H

#include "models/AutoEqService.h"
#include "models/DSPEngineController.h"
#include "models/PipelineStore.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <memory>

class AutoEqPickerDlg : public QDialog {
    Q_OBJECT

public:
    AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr)
        : AutoEqPickerDlg(pipeline, nullptr, parent) {}
    AutoEqPickerDlg(std::shared_ptr<PipelineStore> pipeline, std::shared_ptr<DSPEngineController> dspController,
                    QWidget* parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void onImportClicked();
    void refreshDatabase();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;
    AutoEqService m_service;
    std::vector<AutoEqIndexEntry> m_entries;

    bool m_isLoading = true;
    bool m_isImporting = false;
    QString m_errorMessage;

    QLineEdit* m_searchEdit;
    QListWidget* m_listWidget;
    QPushButton* m_refreshBtn;
    QPushButton* m_cancelBtn;

    QWidget* m_overlayWidget;
    QLabel* m_overlayTitle;
    QLabel* m_overlaySubtitle;

    void setupUi();
    void loadDatabase(bool forceRefresh = false);
    void updateUiState();
};

#endif // AUTO_EQ_PICKER_DLG_H
