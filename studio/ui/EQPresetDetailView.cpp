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
#include "ui/AutoEqPickerDlg.h"
#include "ui/OratoryPresetPickerDlg.h"
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

void EQPresetDetailView::setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum) {
    if (m_diagramWidget) {
        m_diagramWidget->setSpectrumEngine(spectrum);
    }
}

void EQPresetDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Header Toolbar
    auto headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(12);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setFont(QFont("sans-serif", 13, QFont::Bold));
    m_nameEdit->setMaximumWidth(220);
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        m_preset.name = m_nameEdit->text().toStdString();
        m_pipeline->updateEQPreset(m_preset);
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addWidget(new QLabel("Preamp:", this));

    m_preampSlider = new QSlider(Qt::Horizontal, this);
    m_preampSlider->setRange(-360, 360);
    m_preampSlider->setFixedWidth(100);

    m_preampSpin = new QDoubleSpinBox(this);
    m_preampSpin->setRange(-36.0, 36.0);
    m_preampSpin->setSingleStep(0.5);
    m_preampSpin->setSuffix(" dB");
    m_preampSpin->setFixedWidth(80);

    connect(m_preampSlider, &QSlider::valueChanged, [this](int val) {
        double db = val / 10.0;
        if (std::abs(m_preampSpin->value() - db) > 0.05) {
            m_preampSpin->setValue(db);
        }
        m_preset.preampGain = db;
        m_diagramWidget->setPreset(m_preset);
        m_pipeline->updateEQPreset(m_preset);
    });

    connect(m_preampSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double val) {
        int sliderVal = static_cast<int>(std::round(val * 10.0));
        if (m_preampSlider->value() != sliderVal) {
            m_preampSlider->setValue(sliderVal);
        }
        m_preset.preampGain = val;
        m_diagramWidget->setPreset(m_preset);
        m_pipeline->updateEQPreset(m_preset);
    });

    headerLayout->addWidget(m_preampSlider);
    headerLayout->addWidget(m_preampSpin);

    auto showAnalyzerCheck = new QCheckBox("Analyzer", this);
    showAnalyzerCheck->setChecked(true);
    connect(showAnalyzerCheck, &QCheckBox::toggled, [this](bool checked) {
        m_diagramWidget->setShowAnalyzer(checked);
    });
    headerLayout->addWidget(showAnalyzerCheck);

    auto showLoudnessCheck = new QCheckBox("Loudness Contour", this);
    connect(showLoudnessCheck, &QCheckBox::toggled, [this](bool checked) {
        m_diagramWidget->setShowLoudnessContour(checked);
    });
    headerLayout->addWidget(showLoudnessCheck);

    headerLayout->addStretch();

    // Segmented Picker via QTabBar
    m_modeTabBar = new QTabBar(this);
    m_modeTabBar->addTab("📈 Diagram");
    m_modeTabBar->addTab("🎛️ Form");
    m_modeTabBar->addTab("📄 CSV Syntax");
    m_modeTabBar->setDrawBase(false);
    m_modeTabBar->setStyleSheet(
        "QTabBar::tab { background: #2c2c2e; color: #8e8e93; padding: 6px 14px; border-radius: 6px; font-weight: bold; margin-right: 4px; }"
        "QTabBar::tab:selected { background: #007af5; color: white; }"
    );
    connect(m_modeTabBar, &QTabBar::currentChanged, [this](int idx) {
        m_modeStack->setCurrentIndex(idx);
        if (idx == 2) m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
    });
    headerLayout->addWidget(m_modeTabBar);

    mainLayout->addLayout(headerLayout);

    // Secondary Toolbar (Preset Buttons)
    auto subToolLayout = new QHBoxLayout();
    subToolLayout->setSpacing(8);

    auto addBtn = new QPushButton("➕ Add Band", this);
    connect(addBtn, &QPushButton::clicked, this, &EQPresetDetailView::onAddBand);
    subToolLayout->addWidget(addBtn);

    auto autoEqBtn = new QPushButton("🔍 AutoEQ Database", this);
    connect(autoEqBtn, &QPushButton::clicked, [this]() {
        AutoEqPickerDlg dlg(m_pipeline, this);
        dlg.exec();
        refreshUi();
    });
    subToolLayout->addWidget(autoEqBtn);

    auto oratoryBtn = new QPushButton("🎧 Oratory1990", this);
    connect(oratoryBtn, &QPushButton::clicked, [this]() {
        OratoryPresetPickerDlg dlg(m_pipeline, this);
        dlg.exec();
        refreshUi();
    });
    subToolLayout->addWidget(oratoryBtn);

    subToolLayout->addStretch();

    auto expBtn = new QPushButton("Export CSV", this);
    connect(expBtn, &QPushButton::clicked, this, &EQPresetDetailView::onExportCSV);
    subToolLayout->addWidget(expBtn);

    auto impBtn = new QPushButton("Import CSV", this);
    connect(impBtn, &QPushButton::clicked, this, &EQPresetDetailView::onImportCSV);
    subToolLayout->addWidget(impBtn);

    mainLayout->addLayout(subToolLayout);

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
    m_diagramWidget->onBandSelected = [this](int idx) {
        if (idx >= 0 && idx < m_bandsTable->rowCount()) {
            m_bandsTable->selectRow(idx);
        } else {
            m_bandsTable->clearSelection();
        }
    };
    m_modeStack->addWidget(m_diagramWidget);

    // Mode 1: Bands Form Table
    m_bandsTable = new QTableWidget(this);
    m_bandsTable->setColumnCount(7);
    m_bandsTable->setHorizontalHeaderLabels({"Enable", "#", "Type", "Frequency (Hz)", "Gain (dB)", "Q Factor / Slope", "Action"});
    m_bandsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_bandsTable->verticalHeader()->setVisible(false);
    m_bandsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(m_bandsTable, &QTableWidget::itemSelectionChanged, [this]() {
        auto items = m_bandsTable->selectedItems();
        if (!items.isEmpty()) {
            int row = items[0]->row();
            m_diagramWidget->setSelectedBandIndex(row);
        }
    });
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
    applyCsvBtn->setStyleSheet("background-color: #007af5; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");
    connect(applyCsvBtn, &QPushButton::clicked, this, &EQPresetDetailView::onApplyCSV);
    csvBtnRow->addWidget(applyCsvBtn);

    csvLayout->addLayout(csvBtnRow);
    m_modeStack->addWidget(csvWidget);

    mainLayout->addWidget(m_modeStack);
}

void EQPresetDetailView::refreshUi() {
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
    m_preampSpin->setValue(m_preset.preampGain);
    m_preampSlider->setValue(static_cast<int>(std::round(m_preset.preampGain * 10.0)));
    m_diagramWidget->setPreset(m_preset);

    m_bandsTable->setRowCount(0);
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        auto& b = m_preset.bands[i];
        int row = m_bandsTable->rowCount();
        m_bandsTable->insertRow(row);

        auto checkWidget = new QWidget(this);
        auto checkLayout = new QHBoxLayout(checkWidget);
        checkLayout->setContentsMargins(0, 0, 0, 0);
        checkLayout->setAlignment(Qt::AlignCenter);
        auto check = new QCheckBox(checkWidget);
        check->setChecked(b.isEnabled);
        connect(check, &QCheckBox::toggled, [this, i](bool checked) {
            m_preset.bands[i].isEnabled = checked;
            m_diagramWidget->setPreset(m_preset);
            m_pipeline->updateEQPreset(m_preset);
        });
        checkLayout->addWidget(check);
        m_bandsTable->setCellWidget(row, 0, checkWidget);

        auto itemIndex = new QTableWidgetItem(QString::number(i + 1));
        itemIndex->setTextAlignment(Qt::AlignCenter);
        m_bandsTable->setItem(row, 1, itemIndex);

        auto typeCombo = new QComboBox(this);
        for (EQBandType t : {
            EQBandType::Peaking, EQBandType::Lowshelf, EQBandType::Highshelf,
            EQBandType::Lowpass, EQBandType::Highpass, EQBandType::LowpassFO,
            EQBandType::HighpassFO, EQBandType::LowshelfFO, EQBandType::HighshelfFO,
            EQBandType::Notch, EQBandType::Bandpass, EQBandType::Allpass,
            EQBandType::AllpassFO, EQBandType::Free, EQBandType::GeneralNotch,
            EQBandType::LinkwitzTransform
        }) {
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

        // Freq / Coefficients Widget
        if (b.type == EQBandType::Free) {
            auto freeBox = new QHBoxLayout();
            freeBox->setContentsMargins(2, 2, 2, 2);
            freeBox->setSpacing(4);
            auto makeCoeff = [this, i](const QString& label, double val, std::function<void(double)> setter) {
                auto container = new QWidget(this);
                auto l = new QHBoxLayout(container);
                l->setContentsMargins(0, 0, 0, 0);
                l->setSpacing(2);
                l->addWidget(new QLabel(label, container));
                auto spin = new QDoubleSpinBox(container);
                spin->setRange(-100.0, 100.0);
                spin->setSingleStep(0.01);
                spin->setValue(val);
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, setter](double v) {
                    setter(v);
                    m_diagramWidget->setPreset(m_preset);
                    m_pipeline->updateEQPreset(m_preset);
                });
                l->addWidget(spin);
                return container;
            };
            freeBox->addWidget(makeCoeff("b0:", b.b0, [this, i](double v){ m_preset.bands[i].b0 = v; }));
            freeBox->addWidget(makeCoeff("b1:", b.b1, [this, i](double v){ m_preset.bands[i].b1 = v; }));
            freeBox->addWidget(makeCoeff("b2:", b.b2, [this, i](double v){ m_preset.bands[i].b2 = v; }));
            freeBox->addWidget(makeCoeff("a1:", b.a1, [this, i](double v){ m_preset.bands[i].a1 = v; }));
            freeBox->addWidget(makeCoeff("a2:", b.a2, [this, i](double v){ m_preset.bands[i].a2 = v; }));
            auto w = new QWidget(this); w->setLayout(freeBox);
            m_bandsTable->setCellWidget(row, 3, w);
        } else if (b.type == EQBandType::GeneralNotch) {
            auto notchBox = new QHBoxLayout();
            notchBox->setContentsMargins(2, 2, 2, 2);
            notchBox->setSpacing(4);
            auto fcSpin = new QDoubleSpinBox(this); fcSpin->setRange(10, 24000); fcSpin->setValue(b.freqNotch);
            connect(fcSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].freqNotch = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            notchBox->addWidget(new QLabel("Fc:")); notchBox->addWidget(fcSpin);

            auto fpSpin = new QDoubleSpinBox(this); fpSpin->setRange(10, 24000); fpSpin->setValue(b.freqPole);
            connect(fpSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].freqPole = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            notchBox->addWidget(new QLabel("Fp:")); notchBox->addWidget(fpSpin);

            auto normCheck = new QCheckBox("Norm", this); normCheck->setChecked(b.normalizeAtDc);
            connect(normCheck, &QCheckBox::toggled, [this, i](bool chk){ m_preset.bands[i].normalizeAtDc = chk; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            notchBox->addWidget(normCheck);

            auto w = new QWidget(this); w->setLayout(notchBox);
            m_bandsTable->setCellWidget(row, 3, w);
        } else if (b.type == EQBandType::LinkwitzTransform) {
            auto ltBox = new QHBoxLayout();
            ltBox->setContentsMargins(2, 2, 2, 2);
            ltBox->setSpacing(4);
            auto faSpin = new QDoubleSpinBox(this); faSpin->setRange(1, 1000); faSpin->setValue(b.freqAct);
            connect(faSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].freqAct = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            ltBox->addWidget(new QLabel("Fa:")); ltBox->addWidget(faSpin);

            auto qaSpin = new QDoubleSpinBox(this); qaSpin->setRange(0.1, 10); qaSpin->setValue(b.qAct);
            connect(qaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].qAct = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            ltBox->addWidget(new QLabel("Qa:")); ltBox->addWidget(qaSpin);

            auto ftSpin = new QDoubleSpinBox(this); ftSpin->setRange(1, 1000); ftSpin->setValue(b.freqTarget);
            connect(ftSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].freqTarget = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            ltBox->addWidget(new QLabel("Ft:")); ltBox->addWidget(ftSpin);

            auto qtSpin = new QDoubleSpinBox(this); qtSpin->setRange(0.1, 10); qtSpin->setValue(b.qTarget);
            connect(qtSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v){ m_preset.bands[i].qTarget = v; m_diagramWidget->setPreset(m_preset); m_pipeline->updateEQPreset(m_preset); });
            ltBox->addWidget(new QLabel("Qt:")); ltBox->addWidget(qtSpin);

            auto w = new QWidget(this); w->setLayout(ltBox);
            m_bandsTable->setCellWidget(row, 3, w);
        } else {
            auto freqSpin = new QDoubleSpinBox(this);
            freqSpin->setRange(10.0, 24000.0);
            freqSpin->setSingleStep(10.0);
            freqSpin->setValue(b.freq);

            connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].freq = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            m_bandsTable->setCellWidget(row, 3, freqSpin);
        }

        // Gain Widget
        if (eqBandTypeHasGain(b.type)) {
            auto gainSpin = new QDoubleSpinBox(this);
            gainSpin->setRange(-36.0, 36.0);
            gainSpin->setSingleStep(0.5);
            gainSpin->setValue(b.gain);
            connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].gain = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            m_bandsTable->setCellWidget(row, 4, gainSpin);
        } else {
            auto naLabel = new QLabel("N/A", this);
            naLabel->setAlignment(Qt::AlignCenter);
            m_bandsTable->setCellWidget(row, 4, naLabel);
        }

        // Q Widget
        if (eqBandTypeHasQ(b.type)) {
            auto qBox = new QHBoxLayout();
            qBox->setContentsMargins(2, 2, 2, 2);
            auto qSpin = new QDoubleSpinBox(this);
            qSpin->setRange(0.1, 20.0);
            qSpin->setSingleStep(0.05);
            qSpin->setValue(b.useSlope ? b.slope : (b.useBandwidth ? b.bandwidth : b.q));

            connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                auto& band = m_preset.bands[i];
                if (band.useSlope) band.slope = val;
                else if (band.useBandwidth) band.bandwidth = val;
                else band.q = val;
                m_diagramWidget->setPreset(m_preset);
                m_pipeline->updateEQPreset(m_preset);
            });
            qBox->addWidget(qSpin);

            if (b.type == EQBandType::Lowshelf || b.type == EQBandType::Highshelf) {
                auto toggleBtn = new QPushButton(b.useSlope ? "dB/o" : "Q", this);
                toggleBtn->setFixedWidth(45);
                connect(toggleBtn, &QPushButton::clicked, [this, i]() {
                    m_preset.bands[i].useSlope = !m_preset.bands[i].useSlope;
                    refreshUi();
                });
                qBox->addWidget(toggleBtn);
            } else if (b.type == EQBandType::Notch || b.type == EQBandType::Bandpass || b.type == EQBandType::Allpass) {
                auto toggleBtn = new QPushButton(b.useBandwidth ? "oct" : "Q", this);
                toggleBtn->setFixedWidth(45);
                connect(toggleBtn, &QPushButton::clicked, [this, i]() {
                    m_preset.bands[i].useBandwidth = !m_preset.bands[i].useBandwidth;
                    refreshUi();
                });
                qBox->addWidget(toggleBtn);
            }

            auto w = new QWidget(this); w->setLayout(qBox);
            m_bandsTable->setCellWidget(row, 5, w);
        } else {
            auto naLabel = new QLabel("N/A", this);
            naLabel->setAlignment(Qt::AlignCenter);
            m_bandsTable->setCellWidget(row, 5, naLabel);
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
            } else {
                QMessageBox::warning(this, "Import Error", "Failed to parse CSV file format.");
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

