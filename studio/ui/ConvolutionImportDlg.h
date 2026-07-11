#ifndef CONVOLUTION_IMPORT_DLG_H
#define CONVOLUTION_IMPORT_DLG_H

#include "models/PipelineStore.h"
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <memory>

#include <QTableWidget>

class ConvolutionImportDlg : public QDialog {
    Q_OBJECT

public:
    ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);

private slots:
    void onAddFilesClicked();
    void onImportClicked();
    void updateTable();

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
    QTableWidget* m_fileTable;
    QLabel* m_warningLabel;
    QLabel* m_infoLabel;
    QPushButton* m_importBtn;

    void setupUi();
};

#endif // CONVOLUTION_IMPORT_DLG_H
