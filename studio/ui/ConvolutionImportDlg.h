#ifndef CONVOLUTION_IMPORT_DLG_H
#define CONVOLUTION_IMPORT_DLG_H

#include "models/PipelineStore.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <memory>
#include <vector>

class ConvolutionImportDlg : public QDialog {
    Q_OBJECT

public:
    ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);

private slots:
    void onAddFilesClicked();
    void onImportClicked();
    void updateItemsList();

private:
    std::shared_ptr<PipelineStore> m_pipeline;

    struct ImportItem {
        QString filePath;
        int sampleRate = 48000;
        QString format = "WAV";
        int channel = 0;
    };

    std::vector<ImportItem> m_items;

    QLineEdit* m_nameEdit;
    QLineEdit* m_kindEdit;
    QVBoxLayout* m_itemListLayout;
    QWidget* m_emptyStateWidget;
    QLabel* m_warningLabel;
    QWidget* m_errorWidget;
    QLabel* m_errorLabel;
    QPushButton* m_importBtn;

    bool m_isImporting = false;

    void setupUi();
    bool hasDuplicateRates() const;
    void updateImportButtonState();
};

#endif // CONVOLUTION_IMPORT_DLG_H
