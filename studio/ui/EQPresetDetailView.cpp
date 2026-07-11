#include "ui/EQPresetDetailView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QCheckBox>
#include <QClipboard>
#include <QApplication>
#include <fstream>
#include <sstream>

EQPresetDetailView::EQPresetDetailView(
    EQPreset preset,
    std::shared_ptr<PipelineStore> pipeline,
    QWidget* parent
) : QWidget(parent), m_preset(preset), m_pipeline(pipeline) {
    setupUi();
    refreshUi();
}

void EQPresetDetailView::setPreset(const EQPreset& preset) {
    m_preset = preset;
    refreshUi();
}

void EQPresetDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(16);

    // Header Toolbar
    auto headerLayout = new QHBoxLayout();

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setFont(QFont("sans-serif", 14, QFont::Bold));
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        m_preset.name = m_nameEdit->text().toStdString();
        m_pipeline->updateEQPreset(m_preset);
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addStretch();

    headerLayout->addWidget(new QLabel("Preamp Gain:", this));
    m_preampSpin = new QDoubleSpinBox(this);
    m_preampSpin->setRange(-36.0, 36.0);
    m_preampSpin->setSingleStep(0.5);
    m_preampSpin->setSuffix(" dB");
    connect(m_preampSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double val) {
        m_preset.preampGain = val;
        m_diagramWidget->setPreset(m_preset);
        m_pipeline->updateEQPreset(m_preset);
    });
    headerLayout->addWidget(m_preampSpin);

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({"Diagram Graph", "Bands Form Table", "EqualizerAPO Text"});
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_modeStack->setCurrentIndex(idx);
        if (idx == 2) m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
    });
    headerLayout->addWidget(m_modeCombo);

    auto addBtn = new QPushButton("Add Band", this);
    connect(addBtn, &QPushButton::clicked, this, &EQPresetDetailView::onAddBand);
    headerLayout->addWidget(addBtn);

    auto expBtn = new QPushButton("Export CSV", this);
    connect(expBtn, &QPushButton::clicked, this, &EQPresetDetailView::onExportCSV);
    headerLayout->addWidget(expBtn);

    auto impBtn = new QPushButton("Import CSV", this);
    connect(impBtn, &QPushButton::clicked, this, &EQPresetDetailView::onImportCSV);
    headerLayout->addWidget(impBtn);

    mainLayout->addLayout(headerLayout);

    // Mode Stack
    m_modeStack = new QStackedWidget(this);

    // Mode 0: Interactive Diagram
    m_diagramWidget = new EQDiagramWidget(this);
    m_diagramWidget->onBandDragged = [this](int idx, double f, double g) {
        if (idx >= 0 && idx < static_cast<int>(m_preset.bands.size())) {
            auto& b = m_preset.bands[idx];
            if (b.type == EQBandType::GeneralNotch) b.freqNotch = f;
            else if (b.type == EQBandType::LinkwitzTransform) b.freqTarget = f;
            else b.freq = f;

            if (eqBandTypeHasGain(b.type)) b.gain = g;

            m_diagramWidget->setPreset(m_preset);
            m_pipeline->updateEQPreset(m_preset);
            refreshUi();
        }
    };
    m_diagramWidget->onBandQChanged = [this](int idx, double q) {
        if (idx >= 0 && idx < static_cast<int>(m_preset.bands.size())) {
            m_preset.bands[idx].q = q;
            m_diagramWidget->setPreset(m_preset);
            m_pipeline->updateEQPreset(m_preset);
            refreshUi();
        }
    };
    m_modeStack->addWidget(m_diagramWidget);

    // Mode 1: Bands Form Table
    m_bandsTable = new QTableWidget(this);
    m_bandsTable->setColumnCount(7);
    m_bandsTable->setHorizontalHeaderLabels({"Enable", "#", "Type", "Frequency (Hz)", "Gain (dB)", "Q Factor / Slope", "Action"});
    m_bandsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_bandsTable->verticalHeader()->setVisible(false);
    m_modeStack->addWidget(m_bandsTable);

    // Mode 2: EqualizerAPO Text Editor
    auto csvWidget = new QWidget(this);
    auto csvLayout = new QVBoxLayout(csvWidget);

    m_csvTextEdit = new QTextEdit(csvWidget);
    m_csvTextEdit->setFont(QFont("monospace", 11));
    csvLayout->addWidget(m_csvTextEdit);

    auto csvBtnRow = new QHBoxLayout();
    m_csvStatusLabel = new QLabel("AutoEq / EqualizerAPO Syntax", csvWidget);
    csvBtnRow->addWidget(m_csvStatusLabel);
    csvBtnRow->addStretch();

    auto refreshCsvBtn = new QPushButton("Refresh Text", csvWidget);
    connect(refreshCsvBtn, &QPushButton::clicked, [this]() {
        m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
        m_csvStatusLabel->setText("Refreshed from current EQ parameters.");
    });
    csvBtnRow->addWidget(refreshCsvBtn);

    auto copyBtn = new QPushButton("Copy Text", csvWidget);
    connect(copyBtn, &QPushButton::clicked, this, &EQPresetDetailView::onCopyCSV);
    csvBtnRow->addWidget(copyBtn);

    auto applyCsvBtn = new QPushButton("Apply CSV Text", csvWidget);
    applyCsvBtn->setStyleSheet("background-color: #007af5; color: white;");
    connect(applyCsvBtn, &QPushButton::clicked, this, &EQPresetDetailView::onApplyCSV);
    csvBtnRow->addWidget(applyCsvBtn);

    csvLayout->addLayout(csvBtnRow);
    m_modeStack->addWidget(csvWidget);

    mainLayout->addWidget(m_modeStack);
}

void EQPresetDetailView::refreshUi() {
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
    m_preampSpin->setValue(m_preset.preampGain);
    m_diagramWidget->setPreset(m_preset);

    m_bandsTable->setRowCount(0);
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        auto& b = m_preset.bands[i];
        int row = m_bandsTable->rowCount();
        m_bandsTable->insertRow(row);

        auto check = new QCheckBox(this);
        check->setChecked(b.isEnabled);
        connect(check, &QCheckBox::toggled, [this, i](bool checked) {
            m_preset.bands[i].isEnabled = checked;
            m_diagramWidget->setPreset(m_preset);
            m_pipeline->updateEQPreset(m_preset);
        });
        m_bandsTable->setCellWidget(row, 0, check);

        m_bandsTable->setItem(row, 1, new QTableWidgetItem(QString::number(i + 1)));

        auto typeCombo = new QComboBox(this);
        for (EQBandType t : {EQBandType::Peaking, EQBandType::Lowshelf, EQBandType::Highshelf, EQBandType::Lowpass, EQBandType::Highpass, EQBandType::Notch, EQBandType::Bandpass, EQBandType::Allpass, EQBandType::Free, EQBandType::GeneralNotch, EQBandType::LinkwitzTransform}) {
            typeCombo->addItem(QString::fromStdString(eqBandTypeToString(t)), static_cast<int>(t));
        }
        int typeIdx = typeCombo->findData(static_cast<int>(b.type));
        if (typeIdx >= 0) typeCombo->setCurrentIndex(typeIdx);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, typeCombo]() {
            m_preset.bands[i].type = static_cast<EQBandType>(typeCombo->currentData().toInt());
            m_diagramWidget->setPreset(m_preset);
            m_pipeline->updateEQPreset(m_preset);
            refreshUi();
        });
        m_bandsTable->setCellWidget(row, 2, typeCombo);

        // Freq Widget
        if (b.type == EQBandType::Free) {
            auto label = new QLabel("b0,b1,b2,a1,a2", this);
            m_bandsTable->setCellWidget(row, 3, label);
        } else {
            auto freqSpin = new QDoubleSpinBox(this);
            freqSpin->setRange(10.0, 24000.0);
            double currentFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch) currentFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform) currentFreq = b.freqTarget;
            freqSpin->setValue(currentFreq);

            connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                auto& band = m_preset.bands[i];
                if (band.type == EQBandType::GeneralNotch) band.freqNotch = val;
                else if (band.type == EQBandType::LinkwitzTransform) band.freqTarget = val;
                else band.freq = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            m_bandsTable->setCellWidget(row, 3, freqSpin);
        }

        // Gain Widget
        if (eqBandTypeHasGain(b.type)) {
            auto gainSpin = new QDoubleSpinBox(this);
            gainSpin->setRange(-36.0, 36.0);
            gainSpin->setValue(b.gain);
            connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].gain = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            m_bandsTable->setCellWidget(row, 4, gainSpin);
        } else {
            m_bandsTable->setCellWidget(row, 4, new QLabel("N/A", this));
        }

        // Q Widget
        if (eqBandTypeHasQ(b.type)) {
            auto qSpin = new QDoubleSpinBox(this);
            qSpin->setRange(0.1, 20.0);
            qSpin->setSingleStep(0.05);
            qSpin->setValue(b.q);
            connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].q = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            m_bandsTable->setCellWidget(row, 5, qSpin);
        } else {
            m_bandsTable->setCellWidget(row, 5, new QLabel("N/A", this));
        }

        auto delBtn = new QPushButton("Delete", this);
        connect(delBtn, &QPushButton::clicked, [this, row]() {
            onDeleteBand(row);
        });
        m_bandsTable->setCellWidget(row, 6, delBtn);
    }
}

void EQPresetDetailView::onAddBand() {
    m_preset.addBand(EQBand(EQBandType::Peaking, 1000.0, 0.0, 1.41));
    m_pipeline->updateEQPreset(m_preset);
    refreshUi();
}

void EQPresetDetailView::onDeleteBand(int row) {
    m_preset.removeBand(row);
    m_pipeline->updateEQPreset(m_preset);
    refreshUi();
}

void EQPresetDetailView::onExportCSV() {
    QString path = QFileDialog::getSaveFileName(this, "Export EqualizerAPO CSV", "", "CSV Files (*.txt *.csv)");
    if (!path.isEmpty()) {
        std::ofstream file(path.toStdString());
        if (file.is_open()) {
            file << m_preset.toCSV();
            QMessageBox::information(this, "Export Success", "Preset exported successfully.");
        }
    }
}

void EQPresetDetailView::onImportCSV() {
    QString path = QFileDialog::getOpenFileName(this, "Import EqualizerAPO CSV", "", "CSV Files (*.txt *.csv)");
    if (!path.isEmpty()) {
        std::ifstream file(path.toStdString());
        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            auto imported = EQPreset::fromCSV(ss.str(), m_preset.name);
            if (imported.has_value()) {
                m_preset = imported.value();
                m_pipeline->updateEQPreset(m_preset);
                refreshUi();
            }
        }
    }
}

void EQPresetDetailView::onApplyCSV() {
    auto parsed = EQPreset::fromCSV(m_csvTextEdit->toPlainText().toStdString(), m_preset.name);
    if (parsed.has_value()) {
        m_preset = parsed.value();
        m_pipeline->updateEQPreset(m_preset);
        m_csvStatusLabel->setText("Applied EqualizerAPO syntax successfully!");
        refreshUi();
    } else {
        m_csvStatusLabel->setText("Parse error: invalid EqualizerAPO format.");
    }
}

void EQPresetDetailView::onCopyCSV() {
    QApplication::clipboard()->setText(m_csvTextEdit->toPlainText());
    m_csvStatusLabel->setText("Copied to clipboard!");
}
