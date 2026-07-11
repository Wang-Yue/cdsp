#include "ui/ConvolutionPresetDetailView.h"

#include "ui/StyleTheme.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

ConvolutionPresetDetailView::ConvolutionPresetDetailView(ConvolutionPreset preset,
                                                         std::shared_ptr<PipelineStore> pipeline,
                                                         std::shared_ptr<AudioDeviceManager> devices, QWidget* parent)
    : QWidget(parent), m_preset(preset), m_pipeline(pipeline), m_devices(devices) {
    setupUi();
    refreshUi();
}

void ConvolutionPresetDetailView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(16);

    // Header Toolbar
    auto headerLayout = new QHBoxLayout();
    m_nameEdit = new QLineEdit(container);
    m_nameEdit->setFont(QFont("sans-serif", 14, QFont::Bold));
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        m_preset.name = m_nameEdit->text().toStdString();
        m_pipeline->updateConvPreset(m_preset);
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addStretch();

    auto delBtn = new QPushButton("Delete Preset", container);
    connect(delBtn, &QPushButton::clicked, this, &ConvolutionPresetDetailView::onDeleteClicked);
    headerLayout->addWidget(delBtn);

    mainLayout->addLayout(headerLayout);

    // Details Group
    auto detailsGroup = new QGroupBox("Preset Details", container);
    auto form = new QFormLayout(detailsGroup);

    m_kindLabel = new QLabel(detailsGroup);
    form->addRow("Filter Kind:", m_kindLabel);
    m_tapsLabel = new QLabel(detailsGroup);
    form->addRow("Tap Count:", m_tapsLabel);
    m_ratesLabel = new QLabel(detailsGroup);
    form->addRow("Available Rates:", m_ratesLabel);
    m_latencyLabel = new QLabel(detailsGroup);
    form->addRow("Latency:", m_latencyLabel);

    mainLayout->addWidget(detailsGroup);

    // Impulse Response Group
    auto irGroup = new QGroupBox("Impulse Response Plot", container);
    auto irLayout = new QVBoxLayout(irGroup);

    auto rateBox = new QHBoxLayout();
    rateBox->addWidget(new QLabel("Preview Rate:", irGroup));
    m_ratePreviewCombo = new QComboBox(irGroup);
    connect(m_ratePreviewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        m_previewRate = m_ratePreviewCombo->currentData().toInt();
        std::string p = m_preset.irPath(m_previewRate);
        if (!p.empty()) {
            m_irPlot->setIRPath(p);
        }
        double ms = m_preset.latencyMilliseconds(m_previewRate);
        m_latencyLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
    });
    rateBox->addWidget(m_ratePreviewCombo);
    rateBox->addStretch();

    irLayout->addLayout(rateBox);

    m_irPlot = new ConvolutionIRPlot(irGroup);
    irLayout->addWidget(m_irPlot);

    mainLayout->addWidget(irGroup);

    // Files Group
    auto filesGroup = new QGroupBox("On-Disk Files", container);
    auto filesLayout = new QVBoxLayout(filesGroup);

    m_filesContainer = new QWidget(filesGroup);
    m_filesContainer->setLayout(new QVBoxLayout());
    filesLayout->addWidget(m_filesContainer);

    mainLayout->addWidget(filesGroup);
    mainLayout->addStretch();

    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void ConvolutionPresetDetailView::refreshUi() {
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
    m_kindLabel->setText(QString::fromStdString(m_preset.kindLabel()));
    m_tapsLabel->setText(QString::number(m_preset.taps));

    auto rates = m_preset.availableSampleRates();
    QStringList rateStrs;
    for (int r : rates)
        rateStrs.append(QString("%1k").arg(r / 1000));
    m_ratesLabel->setText(rateStrs.join(" / "));

    m_ratePreviewCombo->clear();
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
        if (comboIdx >= 0)
            m_ratePreviewCombo->setCurrentIndex(comboIdx);

        std::string p = m_preset.irPath(m_previewRate);
        if (!p.empty())
            m_irPlot->setIRPath(p);
        double ms = m_preset.latencyMilliseconds(m_previewRate);
        m_latencyLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
    }

    // Populate files list
    auto filesLayout = qobject_cast<QVBoxLayout*>(m_filesContainer->layout());
    QLayoutItem* item;
    while ((item = filesLayout->takeAt(0)) != nullptr) {
        if (item->layout()) {
            QLayoutItem* childItem;
            while ((childItem = item->layout()->takeAt(0)) != nullptr) {
                if (childItem->widget())
                    delete childItem->widget();
                delete childItem;
            }
            delete item->layout();
        } else if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    for (int r : rates) {
        std::string pathStr = m_preset.irPath(r);
        if (!pathStr.empty()) {
            QString p = QString::fromStdString(pathStr);
            auto fileRow = new QHBoxLayout();

            auto rateLbl = new QLabel(QString("%1 Hz:").arg(r), m_filesContainer);
            rateLbl->setFixedWidth(80);
            fileRow->addWidget(rateLbl);

            auto pathLbl = new QLabel(p, m_filesContainer);
            pathLbl->setFont(QFont("monospace", 10));
            fileRow->addWidget(pathLbl, 1);

            auto openBtn = new QPushButton("Reveal", m_filesContainer);
            connect(openBtn, &QPushButton::clicked,
                    [p]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath())); });
            fileRow->addWidget(openBtn);

            filesLayout->addLayout(fileRow);
        }
    }
}

void ConvolutionPresetDetailView::onDeleteClicked() {
    m_pipeline->deleteConvPreset(m_preset.id);
}
