#ifndef ORATORY_PRESET_PICKER_DLG_H
#define ORATORY_PRESET_PICKER_DLG_H

#include "models/DSPEngineController.h"  // for DSPEngineController
#include "models/OratoryPresetService.h" // for OratoryIndexEntry, OratoryPresetService
#include "models/PipelineStore.h"        // for PipelineStore

#include <QDialog>     // for QDialog
#include <QLabel>      // for QLabel
#include <QObject>     // for Q_OBJECT, slots
#include <QPushButton> // for QPushButton
#include <QString>     // for QString
#include <QWidget>     // for QWidget
#include <memory>      // for shared_ptr
#include <vector>      // for vector

class QDialogButtonBox;
class QLineEdit;
class QListWidget;
class QStackedWidget;

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

    QLineEdit* m_searchEdit = nullptr;
    QListWidget* m_listWidget = nullptr;
    QStackedWidget* m_stackedWidget = nullptr;
    QWidget* m_loadingWidget = nullptr;
    QLabel* m_loadingLabel = nullptr;
    QWidget* m_errorWidget = nullptr;
    QLabel* m_errorTitle = nullptr;
    QLabel* m_errorSubtitle = nullptr;
    QWidget* m_emptyWidget = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptySubtitle = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    void setupUi();
    void loadDatabase(bool forceRefresh = false);
    void updateUiState();
};

#endif // ORATORY_PRESET_PICKER_DLG_H
