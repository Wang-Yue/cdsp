#ifndef ORATORY_PRESET_PICKER_DLG_H
#define ORATORY_PRESET_PICKER_DLG_H

#include "models/DSPEngineController.h"
#include "models/OratoryPresetService.h"
#include "models/PipelineStore.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <memory>

class OratoryPresetPickerDlg : public QDialog {
    Q_OBJECT

public:
    OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr)
        : OratoryPresetPickerDlg(pipeline, nullptr, parent) {}
    OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline, std::shared_ptr<DSPEngineController> dspController,
                           QWidget* parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void onImportClicked();
    void refreshDatabase();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;
    OratoryPresetService m_service;
    std::vector<OratoryIndexEntry> m_entries;

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

#endif // ORATORY_PRESET_PICKER_DLG_H
