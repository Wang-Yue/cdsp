#ifndef CONVOLUTION_IMPORT_DLG_H
#define CONVOLUTION_IMPORT_DLG_H

#include "models/PipelineStore.h"
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <memory>

class ConvolutionImportDlg : public QDialog {
    Q_OBJECT

public:
    ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);

private slots:
    void onBrowseClicked();
    void onImportClicked();

private:
    std::shared_ptr<PipelineStore> m_pipeline;

    QLineEdit* m_pathEdit;
    QLineEdit* m_nameEdit;
    QComboBox* m_channelCombo;
    QComboBox* m_formatCombo;
    QComboBox* m_sampleRateCombo;
    QLabel* m_infoLabel;

    void setupUi();
};

#endif // CONVOLUTION_IMPORT_DLG_H
