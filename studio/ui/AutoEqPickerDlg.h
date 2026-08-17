#ifndef AUTO_EQ_PICKER_DLG_H
#define AUTO_EQ_PICKER_DLG_H

#include "models/AutoEqService.h"
#include "models/DSPEngineController.h"
#include "models/PipelineStore.h"

#include <QDialog>
#include <memory>
#include <vector>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

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

#endif // AUTO_EQ_PICKER_DLG_H
