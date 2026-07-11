#include "ui/ConvolutionImportDlg.h"
#include "ui/StyleTheme.h"
#include "models/ConvCoefficientLoader.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>

ConvolutionImportDlg::ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Import Impulse Response (FIR)");
    resize(500, 320);
    setupUi();
}

void ConvolutionImportDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    auto form = new QFormLayout();

    auto pathBox = new QHBoxLayout();
    m_pathEdit = new QLineEdit(this);
    pathBox->addWidget(m_pathEdit);

    auto browseBtn = new QPushButton("Browse...", this);
    connect(browseBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onBrowseClicked);
    pathBox->addWidget(browseBtn);
    form->addRow("File Path:", pathBox);

    m_nameEdit = new QLineEdit(this);
    form->addRow("Preset Name:", m_nameEdit);

    m_channelCombo = new QComboBox(this);
    m_channelCombo->addItems({"Left Channel (Ch 0)", "Right Channel (Ch 1)"});
    form->addRow("Target Channel:", m_channelCombo);

    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItems({"Auto Detect", "WAV Audio File", "Raw Float 64", "Raw Float 32", "Text Sample List"});
    form->addRow("File Format:", m_formatCombo);

    m_sampleRateCombo = new QComboBox(this);
    for (int rate : {44100, 48000, 88200, 96000, 192000}) {
        m_sampleRateCombo->addItem(QString("%1 Hz").arg(rate), rate);
    }
    m_sampleRateCombo->setCurrentIndex(1);
    form->addRow("Target Rate:", m_sampleRateCombo);

    mainLayout->addLayout(form);

    m_infoLabel = new QLabel("Select an IR file to import.", this);
    mainLayout->addWidget(m_infoLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto importBtn = new QPushButton("Import Preset", this);
    connect(importBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onImportClicked);
    btnLayout->addWidget(importBtn);

    auto cancelBtn = new QPushButton("Cancel", this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    mainLayout->addLayout(btnLayout);
}

void ConvolutionImportDlg::onBrowseClicked() {
    QString path = QFileDialog::getOpenFileName(this, "Select IR File", "", "Audio Files (*.wav *.f64 *.f32 *.pcm *.txt)");
    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
        QFileInfo fi(path);
        m_nameEdit->setText(fi.baseName());

        auto wavInfo = ConvCoefficientLoader::parseWavHeader(path.toStdString());
        if (wavInfo.has_value()) {
            m_infoLabel->setText(QString("WAV Header: %1 Hz, %2-bit, %3 channels")
                .arg(wavInfo->sampleRate)
                .arg(wavInfo->bitsPerSample)
                .arg(wavInfo->channels));
        } else {
            m_infoLabel->setText("Raw or Text file loaded.");
        }
    }
}

void ConvolutionImportDlg::onImportClicked() {
    QString path = m_pathEdit->text();
    if (path.isEmpty()) return;

    int sampleRate = m_sampleRateCombo->currentData().toInt();
    int ch = m_channelCombo->currentIndex();

    auto coeffs = ConvCoefficientLoader::loadCoefficients(path.toStdString(), "AUTO", ch, sampleRate);
    if (coeffs.empty()) {
        QMessageBox::warning(this, "Error", "Failed to load coefficients from file.");
        return;
    }

    std::map<int, std::string> paths;
    paths[sampleRate] = path.toStdString();

    ConvolutionPreset preset(m_nameEdit->text().toStdString(), paths, static_cast<int>(coeffs.size()), "Imported IR");
    m_pipeline->addConvPreset(preset);

    QMessageBox::information(this, "Success", QString("Imported IR preset with %1 taps.").arg(coeffs.size()));
    accept();
}
