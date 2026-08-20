#include "ui/ConvolutionPresetDetailView.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

ConvolutionPresetDetailView::ConvolutionPresetDetailView(ConvolutionPreset preset,
                                                         std::shared_ptr<PipelineStore> pipeline,
                                                         std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_preset(preset), m_pipeline(pipeline), m_devices(devices) {
    setupUi();
    refreshUi();

    if (m_pipeline) {
        connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, [this]() {
            for (const auto& p : m_pipeline->convPresets) {
                if (p.id == m_preset.id) {
                    m_preset = p;
                    refreshUi();
                    break;
                }
            }
        });
    }
}

void ConvolutionPresetDetailView::setupUi() {
    auto outerLayout = new QVBoxLayout(this);

    // Header Toolbar
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
        if (m_preset.name != text.toStdString()) {
            m_preset.name = text.toStdString();
            m_pipeline->updateConvPreset(m_preset);
        }
    });
    headerForm->addRow("Preset Name:", m_nameEdit);
    headerLayout->addLayout(headerForm);

    headerLayout->addStretch();

    auto delBtn = new QPushButton("Delete Preset", this);
    connect(delBtn, &QPushButton::clicked, this, &ConvolutionPresetDetailView::onDeleteClicked);
    headerLayout->addWidget(delBtn);

    outerLayout->addLayout(headerLayout);

    // Scroll View
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto container = new QWidget(scroll);
    container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto mainLayout = new QVBoxLayout(container);

    // Preset Properties Group (QFormLayout)
    auto detailsGroup = new QGroupBox("Preset Properties", container);
    detailsGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto detailsForm = new QFormLayout(detailsGroup);
    detailsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_kindLabel = new QLabel(detailsGroup);
    m_kindLabel->setFont(monoFont);
    detailsForm->addRow("Kind:", m_kindLabel);

    m_tapsLabel = new QLabel(detailsGroup);
    m_tapsLabel->setFont(monoFont);
    detailsForm->addRow("Taps:", m_tapsLabel);

    m_ratesLabel = new QLabel(detailsGroup);
    m_ratesLabel->setFont(monoFont);
    detailsForm->addRow("Available Rates:", m_ratesLabel);

    m_latencyKeyLabel = new QLabel("Latency @ 48k:", detailsGroup);
    m_latencyValueLabel = new QLabel(detailsGroup);
    m_latencyValueLabel->setFont(monoFont);
    detailsForm->addRow(m_latencyKeyLabel, m_latencyValueLabel);

    mainLayout->addWidget(detailsGroup);

    // Impulse Response Group
    auto irGroup = new QGroupBox("Impulse Response", container);
    irGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto irLayout = new QVBoxLayout(irGroup);

    m_rateBoxWidget = new QWidget(irGroup);
    auto rateBoxForm = new QFormLayout(m_rateBoxWidget);
    rateBoxForm->setContentsMargins(0, 0, 0, 0);

    m_ratePreviewCombo = new QComboBox(m_rateBoxWidget);
    connect(m_ratePreviewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        if (m_ratePreviewCombo->currentIndex() < 0)
            return;
        m_previewRate = m_ratePreviewCombo->currentData().toInt();
        std::string p = m_preset.irPath(m_previewRate);
        if (!p.empty()) {
            m_irPlot->setIRPath(p);
            m_irPlot->setVisible(true);
            m_noIrLabel->setVisible(false);
        } else {
            m_irPlot->setVisible(false);
            m_noIrLabel->setText(QString("No IR available for %1 Hz.").arg(m_previewRate));
            m_noIrLabel->setVisible(true);
        }
        double ms = m_preset.latencyMilliseconds(m_previewRate);
        m_latencyKeyLabel->setText(QString("Latency @ %1k:").arg(m_previewRate / 1000));
        m_latencyValueLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
    });
    rateBoxForm->addRow("Preview Rate:", m_ratePreviewCombo);
    irLayout->addWidget(m_rateBoxWidget);

    m_irPlot = new ConvolutionIRPlot(irGroup);
    m_irPlot->setFixedHeight(180);
    irLayout->addWidget(m_irPlot);

    m_noIrLabel = new QLabel(irGroup);
    m_noIrLabel->setVisible(false);
    irLayout->addWidget(m_noIrLabel);

    mainLayout->addWidget(irGroup);

    // Sample Rate Files Group (QFormLayout)
    m_filesGroup = new QGroupBox("Sample Rate Files", container);
    m_filesGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_filesForm = new QFormLayout(m_filesGroup);
    m_filesForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    mainLayout->addWidget(m_filesGroup);
    mainLayout->addStretch();

    scroll->setWidget(container);
    outerLayout->addWidget(scroll, 1);
}

void ConvolutionPresetDetailView::refreshUi() {
    m_nameEdit->blockSignals(true);
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
    m_nameEdit->blockSignals(false);
    m_kindLabel->setText(QString::fromStdString(m_preset.kindLabel()));
    m_tapsLabel->setText(QString::number(m_preset.taps));

    auto rates = m_preset.availableSampleRates();
    QStringList rateStrs;
    for (int r : rates)
        rateStrs.append(QString("%1k").arg(r / 1000));
    m_ratesLabel->setText(rates.empty() ? "—" : rateStrs.join(" / "));

    m_ratePreviewCombo->clear();
    m_rateBoxWidget->setVisible(rates.size() > 1);

    for (int r : rates) {
        m_ratePreviewCombo->addItem(QString("%1 Hz").arg(r), r);
    }

    if (!rates.empty()) {
        int liveRate = m_devices ? m_devices->captureConfig.sampleRate : 48000;
        if (std::find(rates.begin(), rates.end(), liveRate) != rates.end()) {
            m_previewRate = liveRate;
        } else {
            double targetLog = std::log(static_cast<double>(liveRate));
            m_previewRate = *std::min_element(rates.begin(), rates.end(), [targetLog](int a, int b) {
                return std::abs(std::log(static_cast<double>(a)) - targetLog) <
                       std::abs(std::log(static_cast<double>(b)) - targetLog);
            });
        }
        int comboIdx = m_ratePreviewCombo->findData(m_previewRate);
        if (comboIdx >= 0) {
            m_ratePreviewCombo->blockSignals(true);
            m_ratePreviewCombo->setCurrentIndex(comboIdx);
            m_ratePreviewCombo->blockSignals(false);
        }

        std::string p = m_preset.irPath(m_previewRate);
        if (!p.empty()) {
            m_irPlot->setIRPath(p);
            m_irPlot->setVisible(true);
            m_noIrLabel->setVisible(false);
        } else {
            m_irPlot->setVisible(false);
            m_noIrLabel->setText(QString("No IR available for %1 Hz.").arg(m_previewRate));
            m_noIrLabel->setVisible(true);
        }
        double ms = m_preset.latencyMilliseconds(m_previewRate);
        m_latencyKeyLabel->setText(QString("Latency @ %1k:").arg(m_previewRate / 1000));
        m_latencyValueLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
    }

    // Populate files list using QFormLayout
    while (QLayoutItem* item = m_filesForm->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (rates.empty()) {
        auto noFilesLbl = new QLabel("No impulse response files associated.", m_filesGroup);
        m_filesForm->addRow(noFilesLbl);
    } else {
        for (int r : rates) {
            std::string pathStr = m_preset.irPath(r);
            if (!pathStr.empty()) {
                QString p = QString::fromStdString(pathStr);
                auto fileRowWidget = new QWidget(m_filesGroup);
                auto fileRowLayout = new QHBoxLayout(fileRowWidget);
                fileRowLayout->setContentsMargins(0, 0, 0, 0);

                auto pathLbl = new QLabel(p, fileRowWidget);
                pathLbl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                pathLbl->setToolTip(p);
                pathLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                pathLbl->setMinimumWidth(0);
                fileRowLayout->addWidget(pathLbl, 1);

                auto openBtn = new QPushButton("Show in Folder", fileRowWidget);
                openBtn->setToolTip("Open containing folder in file manager");
                connect(openBtn, &QPushButton::clicked,
                        [p]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath())); });
                fileRowLayout->addWidget(openBtn);

                m_filesForm->addRow(QString("%1 Hz:").arg(r), fileRowWidget);
            }
        }
    }
}

void ConvolutionPresetDetailView::onDeleteClicked() {
    m_pipeline->deleteConvPreset(m_preset.id);
}
