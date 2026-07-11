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
    OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
        : OratoryPresetPickerDlg(pipeline, nullptr, parent) {}
    OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline, std::shared_ptr<DSPEngineController> dspController,
                           QWidget* parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void onImportClicked();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;
    OratoryPresetService m_service;
    std::vector<OratoryIndexEntry> m_entries;

    QLineEdit* m_searchEdit;
    QListWidget* m_listWidget;
    QPushButton* m_importBtn;
    QLabel* m_statusLabel;

    void setupUi();
    void loadIndex(bool forceRefresh = false);
};

#endif // ORATORY_PRESET_PICKER_DLG_H
