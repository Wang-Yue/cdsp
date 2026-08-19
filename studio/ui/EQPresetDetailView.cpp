#include "ui/EQPresetDetailView.h"

#include "ui/AutoEqPickerDlg.h"
#include "ui/OratoryPresetPickerDlg.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <fstream>
#include <sstream>

EQPresetDetailView::EQPresetDetailView(EQPreset preset, std::shared_ptr<PipelineStore> pipeline,
                                       std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QWidget(parent), m_preset(preset), m_pipeline(pipeline), m_dspController(dspController) {
    setupUi();
    refreshUi();

    if (m_pipeline) {
        connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, [this]() {
            if (m_diagramWidget)
                m_diagramWidget->update();
        });
    }
    if (m_dspController && m_dspController->settings()) {
        connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this, [this]() {
            if (m_diagramWidget)
                m_diagramWidget->update();
        });
    }
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

    // Header with preset details using QFormLayout
    auto headerLayout = new QHBoxLayout();

    auto headerForm = new QFormLayout();
    headerForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_nameEdit = new QLineEdit(this);
    QFont nameFont = font();
    nameFont.setPointSize(13);
    nameFont.setBold(true);
    m_nameEdit->setFont(nameFont);
    m_nameEdit->setPlaceholderText("Preset Name");
    connect(m_nameEdit, &QLineEdit::textChanged, [this](const QString& text) {
        if (m_isRefreshing)
            return;
        if (m_preset.name != text.toStdString()) {
            m_preset.name = text.toStdString();
            if (m_pipeline) {
                m_pipeline->updateEQPreset(m_preset);
            }
        }
    });
    headerForm->addRow("Preset Name:", m_nameEdit);
    headerLayout->addLayout(headerForm);
    headerLayout->addStretch();

    mainLayout->addLayout(headerLayout);

    // Native QTabWidget for mode switching
    m_tabWidget = new QTabWidget(this);

    auto syncPreamp = [this](double db) {
        if (m_isRefreshing)
            return;
        m_preset.preampGain = db;
        int sVal = static_cast<int>(std::round(db * 10.0));

        m_isRefreshing = true;
        if (m_preampSlider && m_preampSlider->value() != sVal)
            m_preampSlider->setValue(sVal);
        if (m_preampSpin && std::abs(m_preampSpin->value() - db) > 0.01)
            m_preampSpin->setValue(db);
        if (m_formPreampSlider && m_formPreampSlider->value() != sVal)
            m_formPreampSlider->setValue(sVal);
        if (m_formPreampSpin && std::abs(m_formPreampSpin->value() - db) > 0.01)
            m_formPreampSpin->setValue(db);
        m_isRefreshing = false;

        m_diagramWidget->setPreset(m_preset);
        applyConfig();
    };

    // Mode 0: Interactive Diagram with Preamp Parameter Form & Bottom Band Chips Bar
    auto diagramModeWidget = new QWidget(this);
    auto diagramModeLayout = new QVBoxLayout(diagramModeWidget);

    auto diagParamForm = new QFormLayout();
    diagParamForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto diagPreampWidget = new QWidget(diagramModeWidget);
    auto diagPreampBar = new QHBoxLayout(diagPreampWidget);
    diagPreampBar->setContentsMargins(0, 0, 0, 0);

    m_preampSlider = new QSlider(Qt::Horizontal, diagPreampWidget);
    m_preampSlider->setRange(-200, 120);

    m_preampSpin = new QDoubleSpinBox(diagPreampWidget);
    m_preampSpin->setRange(-20.0, 12.0);
    m_preampSpin->setSingleStep(0.1);
    m_preampSpin->setSuffix(" dB");

    connect(m_preampSlider, &QSlider::valueChanged, [syncPreamp](int val) { syncPreamp(val / 10.0); });
    connect(m_preampSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [syncPreamp](double val) { syncPreamp(val); });

    diagPreampBar->addWidget(m_preampSlider);
    diagPreampBar->addWidget(m_preampSpin);

    QSettings settings;
    bool showAnalyzer = settings.value("eq_show_analyzer", true).toBool();
    bool showLoudness = settings.value("eq_show_loudness_contour", false).toBool();

    auto showAnalyzerCheck = new QCheckBox("Analyzer", diagPreampWidget);
    showAnalyzerCheck->setChecked(showAnalyzer);
    connect(showAnalyzerCheck, &QCheckBox::toggled, [this](bool checked) {
        m_diagramWidget->setShowAnalyzer(checked);
        QSettings s;
        s.setValue("eq_show_analyzer", checked);
    });
    diagPreampBar->addWidget(showAnalyzerCheck);

    auto showLoudnessCheck = new QCheckBox("Loudness Contour", diagPreampWidget);
    showLoudnessCheck->setChecked(showLoudness);
    connect(showLoudnessCheck, &QCheckBox::toggled, [this](bool checked) {
        m_diagramWidget->setShowLoudnessContour(checked);
        QSettings s;
        s.setValue("eq_show_loudness_contour", checked);
    });
    diagPreampBar->addWidget(showLoudnessCheck);
    diagPreampBar->addStretch();

    diagParamForm->addRow("Preamp Gain:", diagPreampWidget);
    diagramModeLayout->addLayout(diagParamForm);

    m_diagramWidget = new EQDiagramWidget(diagramModeWidget);
    m_diagramWidget->setShowAnalyzer(showAnalyzer);
    m_diagramWidget->setShowLoudnessContour(showLoudness);
    m_diagramWidget->setPipelineStore(m_pipeline);
    if (m_dspController) {
        m_diagramWidget->setAudioSettings(m_dspController->settings());
    }
    m_diagramWidget->onPresetChanged = [this]() {
        applyConfig();
        refreshUi();
    };
    m_diagramWidget->onBandAdded = [this](double freq, double gain) {
        applyConfig();
        refreshUi();
    };
    m_diagramWidget->onBandDeleted = [this](int idx) {
        if (idx >= 0 && idx < static_cast<int>(m_preset.bands.size())) {
            m_preset.bands.erase(m_preset.bands.begin() + idx);
            m_diagramWidget->setSelectedBandIndex(-1);
            applyConfig();
            refreshUi();
        }
    };
    m_diagramWidget->onBandDragged = [this](int idx, double f, double g) {
        if (idx >= 0 && idx < static_cast<int>(m_preset.bands.size())) {
            auto& b = m_preset.bands[idx];
            if (b.type == EQBandType::GeneralNotch)
                b.freqNotch = f;
            else if (b.type == EQBandType::LinkwitzTransform)
                b.freqTarget = f;
            else
                b.freq = f;

            if (eqBandTypeHasGain(b.type))
                b.gain = g;

            m_diagramWidget->setPreset(m_preset);
            applyConfig();
            updateBandChipsBar();
        }
    };
    m_diagramWidget->onBandQChanged = [this](int idx, double val) {
        if (idx >= 0 && idx < static_cast<int>(m_preset.bands.size())) {
            auto& band = m_preset.bands[idx];
            if (band.useSlope) {
                band.slope = val;
            } else if (band.useBandwidth) {
                band.bandwidth = val;
            } else {
                band.q = val;
            }
            m_diagramWidget->setPreset(m_preset);
            applyConfig();
            updateBandChipsBar();
        }
    };
    m_diagramWidget->onBandSelected = [this](int idx) {
        if (idx >= 0 && idx < m_bandsTable->rowCount()) {
            m_bandsTable->selectRow(idx);
        } else {
            m_bandsTable->clearSelection();
        }
        updateBandChipsBar();
    };
    diagramModeLayout->addWidget(m_diagramWidget, 1);

    // Bottom Band Chips Bar
    auto chipsBarLayout = new QHBoxLayout();

    auto chipsScroll = new QScrollArea(diagramModeWidget);
    chipsScroll->setWidgetResizable(true);
    chipsScroll->setFixedHeight(62);
    chipsScroll->setFrameShape(QFrame::NoFrame);
    chipsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chipsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_bandChipsWidget = new QWidget(chipsScroll);
    m_chipLayout = new QHBoxLayout(m_bandChipsWidget);
    chipsScroll->setWidget(m_bandChipsWidget);

    chipsBarLayout->addWidget(chipsScroll, 1);

    auto quickAddBtn = new QPushButton("Add Band", diagramModeWidget);
    quickAddBtn->setToolTip("Add new EQ filter band");
    connect(quickAddBtn, &QPushButton::clicked, this, &EQPresetDetailView::onAddBand);
    chipsBarLayout->addWidget(quickAddBtn, 0, Qt::AlignVCenter);

    diagramModeLayout->addLayout(chipsBarLayout);

    m_tabWidget->addTab(diagramModeWidget, "Diagram");

    // Mode 1: Bands Form Mode (Preamp Parameter Form + Bands Table + Add Band Button)
    auto formModeWidget = new QWidget(this);
    auto formModeLayout = new QVBoxLayout(formModeWidget);

    auto formParamForm = new QFormLayout();
    formParamForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto formPreampWidget = new QWidget(formModeWidget);
    auto formPreampBar = new QHBoxLayout(formPreampWidget);
    formPreampBar->setContentsMargins(0, 0, 0, 0);

    m_formPreampSlider = new QSlider(Qt::Horizontal, formPreampWidget);
    m_formPreampSlider->setRange(-200, 120);

    m_formPreampSpin = new QDoubleSpinBox(formPreampWidget);
    m_formPreampSpin->setRange(-20.0, 12.0);
    m_formPreampSpin->setSingleStep(0.1);
    m_formPreampSpin->setSuffix(" dB");

    connect(m_formPreampSlider, &QSlider::valueChanged, [syncPreamp](int val) { syncPreamp(val / 10.0); });
    connect(m_formPreampSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [syncPreamp](double val) { syncPreamp(val); });

    formPreampBar->addWidget(m_formPreampSlider, 1);
    formPreampBar->addWidget(m_formPreampSpin);

    formParamForm->addRow("Preamp Gain:", formPreampWidget);
    formModeLayout->addLayout(formParamForm);

    m_bandsTable = new QTableWidget(formModeWidget);
    m_bandsTable->setColumnCount(7);
    m_bandsTable->setHorizontalHeaderLabels(
        {"Enable", "#", "Type", "Frequency (Hz)", "Gain (dB)", "Q Factor / Slope", "Action"});
    m_bandsTable->horizontalHeader()->setStretchLastSection(false);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_bandsTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_bandsTable->verticalHeader()->setVisible(false);
    m_bandsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bandsTable->setAlternatingRowColors(true);
    connect(m_bandsTable, &QTableWidget::itemSelectionChanged, [this]() {
        auto items = m_bandsTable->selectedItems();
        if (!items.isEmpty()) {
            int row = items[0]->row();
            m_diagramWidget->setSelectedBandIndex(row);
        }
    });
    formModeLayout->addWidget(m_bandsTable, 1);

    auto addBandFormBtn = new QPushButton("Add Band", formModeWidget);
    connect(addBandFormBtn, &QPushButton::clicked, this, &EQPresetDetailView::onAddBand);
    formModeLayout->addWidget(addBandFormBtn, 0, Qt::AlignLeft);

    m_tabWidget->addTab(formModeWidget, "Form");

    // Mode 2: EqualizerAPO Text Editor (EQCSVMode)
    auto csvWidget = new QWidget(this);
    auto csvLayout = new QVBoxLayout(csvWidget);

    auto csvHeaderLayout = new QHBoxLayout();

    auto csvTitleVBox = new QVBoxLayout();
    auto csvTitleLbl = new QLabel("AutoEq / EqualizerAPO format", csvWidget);
    QFont titleFont = font();
    titleFont.setPointSize(12);
    titleFont.setBold(true);
    csvTitleLbl->setFont(titleFont);
    auto csvSubLbl = new QLabel("Edit and Apply, or paste from AutoEq output", csvWidget);
    csvTitleVBox->addWidget(csvTitleLbl);
    csvTitleVBox->addWidget(csvSubLbl);

    csvHeaderLayout->addLayout(csvTitleVBox);
    csvHeaderLayout->addStretch();

    m_csvCopyBtn = new QPushButton("Copy Text", csvWidget);
    connect(m_csvCopyBtn, &QPushButton::clicked, this, &EQPresetDetailView::onCopyCSV);
    csvHeaderLayout->addWidget(m_csvCopyBtn);

    auto refreshCsvBtn = new QPushButton("Refresh", csvWidget);
    connect(refreshCsvBtn, &QPushButton::clicked, [this]() {
        m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
        if (m_csvErrorLabel)
            m_csvErrorLabel->hide();
    });
    csvHeaderLayout->addWidget(refreshCsvBtn);

    auto applyCsvBtn = new QPushButton("Apply", csvWidget);
    applyCsvBtn->setDefault(true);
    connect(applyCsvBtn, &QPushButton::clicked, this, &EQPresetDetailView::onApplyCSV);
    csvHeaderLayout->addWidget(applyCsvBtn);

    csvLayout->addLayout(csvHeaderLayout);

    m_csvErrorLabel = new QLabel(csvWidget);
    m_csvErrorLabel->hide();
    csvLayout->addWidget(m_csvErrorLabel);

    m_csvTextEdit = new QTextEdit(csvWidget);
    m_csvTextEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    csvLayout->addWidget(m_csvTextEdit, 1);

    m_tabWidget->addTab(csvWidget, "CSV");

    connect(m_tabWidget, &QTabWidget::currentChanged, [this](int idx) {
        if (idx == 2) {
            m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
            if (m_csvErrorLabel)
                m_csvErrorLabel->hide();
        }
        refreshUi();
    });

    mainLayout->addWidget(m_tabWidget, 1);
}

void EQPresetDetailView::refreshUi() {
    m_isRefreshing = true;
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
    m_preampSpin->setValue(m_preset.preampGain);
    m_preampSlider->setValue(static_cast<int>(std::round(m_preset.preampGain * 10.0)));
    if (m_formPreampSpin)
        m_formPreampSpin->setValue(m_preset.preampGain);
    if (m_formPreampSlider)
        m_formPreampSlider->setValue(static_cast<int>(std::round(m_preset.preampGain * 10.0)));
    m_diagramWidget->setPreset(m_preset);

    m_bandsTable->setRowCount(0);
    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        auto& b = m_preset.bands[i];
        int row = m_bandsTable->rowCount();
        m_bandsTable->insertRow(row);

        auto checkWidget = new QWidget(m_bandsTable);
        auto checkLayout = new QHBoxLayout(checkWidget);
        checkLayout->setContentsMargins(0, 0, 0, 0);
        checkLayout->setAlignment(Qt::AlignCenter);
        auto check = new QCheckBox(checkWidget);
        check->setChecked(b.isEnabled);
        connect(check, &QCheckBox::toggled, [this, i](bool checked) {
            m_preset.bands[i].isEnabled = checked;
            m_diagramWidget->setPreset(m_preset);
            applyConfig();
        });
        checkLayout->addWidget(check);
        m_bandsTable->setCellWidget(row, 0, checkWidget);

        auto itemIndex = new QTableWidgetItem(QString::number(i + 1));
        itemIndex->setTextAlignment(Qt::AlignCenter);
        m_bandsTable->setItem(row, 1, itemIndex);

        auto typeCombo = new QComboBox(m_bandsTable);
        for (EQBandType t :
             {EQBandType::Peaking, EQBandType::Lowshelf, EQBandType::Highshelf, EQBandType::Lowpass,
              EQBandType::Highpass, EQBandType::LowpassFO, EQBandType::HighpassFO, EQBandType::LowshelfFO,
              EQBandType::HighshelfFO, EQBandType::Notch, EQBandType::Bandpass, EQBandType::Allpass,
              EQBandType::AllpassFO, EQBandType::Free, EQBandType::GeneralNotch, EQBandType::LinkwitzTransform}) {
            typeCombo->addItem(QString::fromStdString(eqBandTypeToString(t)), static_cast<int>(t));
        }
        int typeIdx = typeCombo->findData(static_cast<int>(b.type));
        if (typeIdx >= 0)
            typeCombo->setCurrentIndex(typeIdx);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, typeCombo]() {
            m_preset.bands[i].type = static_cast<EQBandType>(typeCombo->currentData().toInt());
            m_diagramWidget->setPreset(m_preset);
            applyConfig();
            refreshUi();
        });
        m_bandsTable->setCellWidget(row, 2, typeCombo);

        // Freq / Coefficients Widget
        if (b.type == EQBandType::Free) {
            auto freeWidget = new QWidget(m_bandsTable);
            auto freeBox = new QHBoxLayout(freeWidget);
            freeBox->setContentsMargins(0, 0, 0, 0);
            auto makeCoeff = [this, i, freeWidget](const QString& label, double val,
                                                   std::function<void(double)> setter) {
                auto container = new QWidget(freeWidget);
                auto l = new QHBoxLayout(container);
                l->setContentsMargins(0, 0, 0, 0);
                l->addWidget(new QLabel(label, container));
                auto spin = new QDoubleSpinBox(container);
                spin->setRange(-100.0, 100.0);
                spin->setDecimals(6);
                spin->setSingleStep(0.001);
                spin->setValue(val);
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, setter](double v) {
                    setter(v);
                    m_diagramWidget->setPreset(m_preset);
                    applyConfig();
                });
                l->addWidget(spin);
                return container;
            };
            freeBox->addWidget(makeCoeff("b0:", b.b0, [this, i](double v) { m_preset.bands[i].b0 = v; }));
            freeBox->addWidget(makeCoeff("b1:", b.b1, [this, i](double v) { m_preset.bands[i].b1 = v; }));
            freeBox->addWidget(makeCoeff("b2:", b.b2, [this, i](double v) { m_preset.bands[i].b2 = v; }));
            freeBox->addWidget(makeCoeff("a1:", b.a1, [this, i](double v) { m_preset.bands[i].a1 = v; }));
            freeBox->addWidget(makeCoeff("a2:", b.a2, [this, i](double v) { m_preset.bands[i].a2 = v; }));
            m_bandsTable->setCellWidget(row, 3, freeWidget);
        } else if (b.type == EQBandType::GeneralNotch) {
            auto notchWidget = new QWidget(m_bandsTable);
            auto notchBox = new QHBoxLayout(notchWidget);
            notchBox->setContentsMargins(0, 0, 0, 0);
            auto fcSpin = new QDoubleSpinBox(notchWidget);
            fcSpin->setRange(10, 24000);
            fcSpin->setValue(b.freqNotch);
            connect(fcSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].freqNotch = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            notchBox->addWidget(new QLabel("Fc:", notchWidget));
            notchBox->addWidget(fcSpin);

            auto fpSpin = new QDoubleSpinBox(notchWidget);
            fpSpin->setRange(10, 24000);
            fpSpin->setValue(b.freqPole);
            connect(fpSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].freqPole = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            notchBox->addWidget(new QLabel("Fp:", notchWidget));
            notchBox->addWidget(fpSpin);

            auto qpSpin = new QDoubleSpinBox(notchWidget);
            qpSpin->setRange(0.01, 100.0);
            qpSpin->setValue(b.qPole);
            connect(qpSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].qPole = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            notchBox->addWidget(new QLabel("Qp:", notchWidget));
            notchBox->addWidget(qpSpin);

            auto normCheck = new QCheckBox("Norm", notchWidget);
            normCheck->setChecked(b.normalizeAtDc);
            connect(normCheck, &QCheckBox::toggled, [this, i](bool chk) {
                m_preset.bands[i].normalizeAtDc = chk;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            notchBox->addWidget(normCheck);

            m_bandsTable->setCellWidget(row, 3, notchWidget);
        } else if (b.type == EQBandType::LinkwitzTransform) {
            auto ltWidget = new QWidget(m_bandsTable);
            auto ltBox = new QHBoxLayout(ltWidget);
            ltBox->setContentsMargins(0, 0, 0, 0);
            auto faSpin = new QDoubleSpinBox(ltWidget);
            faSpin->setRange(1, 1000);
            faSpin->setValue(b.freqAct);
            connect(faSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].freqAct = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            ltBox->addWidget(new QLabel("Fa:", ltWidget));
            ltBox->addWidget(faSpin);

            auto qaSpin = new QDoubleSpinBox(ltWidget);
            qaSpin->setRange(0.1, 10);
            qaSpin->setValue(b.qAct);
            connect(qaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].qAct = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            ltBox->addWidget(new QLabel("Qa:", ltWidget));
            ltBox->addWidget(qaSpin);

            auto ftSpin = new QDoubleSpinBox(ltWidget);
            ftSpin->setRange(1, 1000);
            ftSpin->setValue(b.freqTarget);
            connect(ftSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].freqTarget = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            ltBox->addWidget(new QLabel("Ft:", ltWidget));
            ltBox->addWidget(ftSpin);

            auto qtSpin = new QDoubleSpinBox(ltWidget);
            qtSpin->setRange(0.1, 10);
            qtSpin->setValue(b.qTarget);
            connect(qtSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double v) {
                m_preset.bands[i].qTarget = v;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            ltBox->addWidget(new QLabel("Qt:", ltWidget));
            ltBox->addWidget(qtSpin);

            m_bandsTable->setCellWidget(row, 3, ltWidget);
        } else {
            auto freqSpin = new QDoubleSpinBox(m_bandsTable);
            freqSpin->setRange(10.0, 24000.0);
            freqSpin->setDecimals(0);
            freqSpin->setSingleStep(10.0);
            freqSpin->setSuffix(" Hz");
            freqSpin->setValue(b.freq);

            connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].freq = val;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            m_bandsTable->setCellWidget(row, 3, freqSpin);
        }

        // Gain Widget
        if (eqBandTypeHasGain(b.type)) {
            auto gainSpin = new QDoubleSpinBox(m_bandsTable);
            gainSpin->setRange(-36.0, 36.0);
            gainSpin->setDecimals(1);
            gainSpin->setSingleStep(0.5);
            gainSpin->setSuffix(" dB");
            gainSpin->setValue(b.gain);
            connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                m_preset.bands[i].gain = val;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            m_bandsTable->setCellWidget(row, 4, gainSpin);
        } else {
            auto naLabel = new QLabel("N/A", m_bandsTable);
            naLabel->setAlignment(Qt::AlignCenter);
            m_bandsTable->setCellWidget(row, 4, naLabel);
        }

        // Q Widget
        if (eqBandTypeHasQ(b.type)) {
            auto qWidget = new QWidget(m_bandsTable);
            auto qBox = new QHBoxLayout(qWidget);
            qBox->setContentsMargins(0, 0, 0, 0);
            auto qSpin = new QDoubleSpinBox(qWidget);
            qSpin->setRange(0.1, 20.0);
            qSpin->setDecimals(2);
            qSpin->setSingleStep(0.05);
            qSpin->setValue(b.useSlope ? b.slope : (b.useBandwidth ? b.bandwidth : b.q));

            connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, i](double val) {
                auto& band = m_preset.bands[i];
                if (band.useSlope)
                    band.slope = val;
                else if (band.useBandwidth)
                    band.bandwidth = val;
                else
                    band.q = val;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
            });
            qBox->addWidget(qSpin);

            if (b.type == EQBandType::Lowshelf || b.type == EQBandType::Highshelf) {
                auto toggleBtn = new QPushButton(b.useSlope ? "dB/o" : "Q", qWidget);
                connect(toggleBtn, &QPushButton::clicked, [this, i]() {
                    m_preset.bands[i].useSlope = !m_preset.bands[i].useSlope;
                    m_diagramWidget->setPreset(m_preset);
                    applyConfig();
                    refreshUi();
                });
                qBox->addWidget(toggleBtn);
            } else if (b.type == EQBandType::Notch || b.type == EQBandType::Bandpass || b.type == EQBandType::Allpass) {
                auto toggleBtn = new QPushButton(b.useBandwidth ? "oct" : "Q", qWidget);
                connect(toggleBtn, &QPushButton::clicked, [this, i]() {
                    m_preset.bands[i].useBandwidth = !m_preset.bands[i].useBandwidth;
                    m_diagramWidget->setPreset(m_preset);
                    applyConfig();
                    refreshUi();
                });
                qBox->addWidget(toggleBtn);
            }

            m_bandsTable->setCellWidget(row, 5, qWidget);
        } else {
            auto naLabel = new QLabel("N/A", m_bandsTable);
            naLabel->setAlignment(Qt::AlignCenter);
            m_bandsTable->setCellWidget(row, 5, naLabel);
        }

        auto delBtn = new QPushButton("Delete", m_bandsTable);
        connect(delBtn, &QPushButton::clicked, [this, row]() { onDeleteBand(row); });
        m_bandsTable->setCellWidget(row, 6, delBtn);
    }
    updateBandChipsBar();
    m_isRefreshing = false;
}

bool EQPresetDetailView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto mouseEv = static_cast<QMouseEvent*>(event);
        if (mouseEv->button() == Qt::LeftButton) {
            QVariant bandIdxVar = watched->property("bandIndex");
            if (bandIdxVar.isValid()) {
                int idx = bandIdxVar.toInt();
                m_diagramWidget->setSelectedBandIndex(idx);
                if (idx >= 0 && idx < m_bandsTable->rowCount()) {
                    m_bandsTable->selectRow(idx);
                }
                updateBandChipsBar();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EQPresetDetailView::updateBandChipsBar() {
    if (!m_chipLayout)
        return;

    // Clear existing chips
    QLayoutItem* item;
    while ((item = m_chipLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    int activeIdx = m_diagramWidget->selectedBandIndex();

    for (size_t i = 0; i < m_preset.bands.size(); ++i) {
        auto& b = m_preset.bands[i];
        int bandIdx = static_cast<int>(i);
        bool isSelected = (bandIdx == activeIdx);
        bool isEnabled = b.isEnabled;

        auto chip = new QFrame(m_bandChipsWidget);
        chip->setFrameShape(isSelected ? QFrame::Box : QFrame::StyledPanel);
        chip->setFrameShadow(isSelected ? QFrame::Plain : QFrame::Raised);
        chip->setLineWidth(isSelected ? 2 : 1);
        chip->setMidLineWidth(0);
        chip->setContextMenuPolicy(Qt::CustomContextMenu);

        QPalette pal = chip->palette();
        if (isSelected) {
            pal.setColor(QPalette::Window, palette().color(QPalette::Highlight));
            pal.setColor(QPalette::WindowText, palette().color(QPalette::HighlightedText));
            pal.setColor(QPalette::Text, palette().color(QPalette::HighlightedText));
            chip->setPalette(pal);
            chip->setAutoFillBackground(true);
        } else {
            chip->setAutoFillBackground(false);
        }

        auto chipHBox = new QHBoxLayout(chip);
        chipHBox->setContentsMargins(6, 4, 6, 4);
        chipHBox->setSpacing(6);

        // Text Content
        auto textVBox = new QVBoxLayout();
        textVBox->setContentsMargins(0, 0, 0, 0);
        textVBox->setSpacing(1);

        // Line 1: #Index Type
        QString typeStr = QString::fromStdString(eqBandTypeToString(b.type));
        QString titleText = isEnabled ? QString("#%1 %2").arg(bandIdx + 1).arg(typeStr)
                                      : QString("#%1 %2 (Disabled)").arg(bandIdx + 1).arg(typeStr);
        auto titleLbl = new QLabel(titleText, chip);
        QFont titleF = font();
        titleF.setPointSize(10);
        titleF.setBold(isSelected);
        titleLbl->setFont(titleF);
        titleLbl->setEnabled(isEnabled);
        titleLbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        if (isSelected) {
            titleLbl->setPalette(pal);
        }
        textVBox->addWidget(titleLbl);

        // Line 2: Freq & Gain / Q
        if (b.type != EQBandType::Free) {
            double displayFreq = b.freq;
            if (b.type == EQBandType::GeneralNotch)
                displayFreq = b.freqNotch;
            else if (b.type == EQBandType::LinkwitzTransform)
                displayFreq = b.freqTarget;

            QString valText = QString("%1Hz").arg(static_cast<int>(std::round(displayFreq)));
            if (eqBandTypeHasGain(b.type)) {
                valText += QString(" %1%2dB").arg(b.gain >= 0 ? "+" : "").arg(b.gain, 0, 'f', 1);
            }
            if (eqBandTypeHasQ(b.type)) {
                if (b.type == EQBandType::GeneralNotch)
                    valText += QString(" Qp:%1").arg(b.qPole, 0, 'f', 2);
                else if (b.type == EQBandType::LinkwitzTransform)
                    valText += QString(" Qt:%1").arg(b.qTarget, 0, 'f', 2);
                else if (b.useSlope)
                    valText += QString(" S:%1").arg(b.slope, 0, 'f', 1);
                else if (b.useBandwidth)
                    valText += QString(" BW:%1").arg(b.bandwidth, 0, 'f', 2);
                else
                    valText += QString(" Q:%1").arg(b.q, 0, 'f', 2);
            }

            auto valLbl = new QLabel(valText, chip);
            valLbl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            valLbl->setEnabled(isEnabled);
            valLbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            if (isSelected) {
                valLbl->setPalette(pal);
            }
            textVBox->addWidget(valLbl);
        }

        chipHBox->addLayout(textVBox);

        chip->installEventFilter(this);
        chip->setProperty("bandIndex", bandIdx);

        // Context Menu
        connect(chip, &QWidget::customContextMenuRequested, [this, bandIdx](const QPoint& pos) {
            auto sourceWidget = qobject_cast<QWidget*>(sender());
            QMenu menu(this);
            auto& band = m_preset.bands[bandIdx];

            auto toggleAction = menu.addAction(band.isEnabled ? "Disable Band" : "Enable Band");
            connect(toggleAction, &QAction::triggered, [this, bandIdx]() {
                m_preset.bands[bandIdx].isEnabled = !m_preset.bands[bandIdx].isEnabled;
                m_diagramWidget->setPreset(m_preset);
                applyConfig();
                refreshUi();
            });

            auto typeMenu = menu.addMenu("Change Type");
            for (EQBandType t :
                 {EQBandType::Peaking, EQBandType::Lowshelf, EQBandType::Highshelf, EQBandType::Lowpass,
                  EQBandType::Highpass, EQBandType::LowpassFO, EQBandType::HighpassFO, EQBandType::LowshelfFO,
                  EQBandType::HighshelfFO, EQBandType::Notch, EQBandType::Bandpass, EQBandType::Allpass,
                  EQBandType::AllpassFO, EQBandType::Free, EQBandType::GeneralNotch, EQBandType::LinkwitzTransform}) {
                auto act = typeMenu->addAction(QString::fromStdString(eqBandTypeToString(t)));
                connect(act, &QAction::triggered, [this, bandIdx, t]() {
                    m_preset.bands[bandIdx].type = t;
                    m_diagramWidget->setPreset(m_preset);
                    applyConfig();
                    refreshUi();
                });
            }

            menu.addSeparator();
            auto delAction = menu.addAction("Delete");
            connect(delAction, &QAction::triggered, [this, bandIdx]() { onDeleteBand(bandIdx); });

            menu.exec(sourceWidget ? sourceWidget->mapToGlobal(pos) : QCursor::pos());
        });

        m_chipLayout->addWidget(chip);
    }

    m_chipLayout->addStretch();
}

void EQPresetDetailView::onAddBand() {
    m_preset.addBand(EQBand(EQBandType::Peaking, 1000.0, 0.0, 1.41));
    int newIndex = static_cast<int>(m_preset.bands.size()) - 1;
    m_diagramWidget->setSelectedBandIndex(newIndex);
    applyConfig();
    refreshUi();
    if (newIndex >= 0 && newIndex < m_bandsTable->rowCount()) {
        m_bandsTable->selectRow(newIndex);
    }
}

void EQPresetDetailView::onDeleteBand(int row) {
    m_preset.removeBand(row);
    applyConfig();
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
                m_preset.preampGain = imported->preampGain;
                m_preset.bands = imported->bands;
                applyConfig();
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
        m_preset.preampGain = parsed->preampGain;
        m_preset.bands = parsed->bands;
        applyConfig();
        if (m_csvErrorLabel)
            m_csvErrorLabel->hide();
        refreshUi();
    } else {
        if (m_csvErrorLabel) {
            m_csvErrorLabel->setText("Failed to parse — check format (expecting 'Filter 1: ON PK Fc...')");
            m_csvErrorLabel->show();
        }
    }
}

void EQPresetDetailView::onCopyCSV() {
    m_csvTextEdit->setText(QString::fromStdString(m_preset.toCSV()));
    QApplication::clipboard()->setText(m_csvTextEdit->toPlainText());
    if (m_csvCopyBtn)
        m_csvCopyBtn->setText("Copied!");
    QTimer::singleShot(1500, this, [this]() {
        if (m_csvCopyBtn)
            m_csvCopyBtn->setText("Copy Text");
    });
}

void EQPresetDetailView::applyConfig() {
    if (m_pipeline) {
        m_pipeline->updateEQPreset(m_preset);
    }
    if (m_dspController) {
        m_dspController->applyConfig();
    }
}
