#include "ui/ConvolutionPresetDetailView.h"

#include "ui/StyleTheme.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QProcess>
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
}

void ConvolutionPresetDetailView::setupUi() {
    auto outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // Header Toolbar
    auto headerWidget = new QWidget(this);
    auto headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 16, 16, 16);
    headerLayout->setSpacing(8);

    auto iconLbl = new QLabel("🔍〰", headerWidget);
    iconLbl->setFont(QFont("sans-serif", 16));
    iconLbl->setStyleSheet(QString("color: %1;").arg(StyleTheme::accent().name()));
    headerLayout->addWidget(iconLbl);

    m_nameEdit = new QLineEdit(headerWidget);
    m_nameEdit->setFont(QFont("sans-serif", 14, QFont::Bold));
    m_nameEdit->setMaximumWidth(300);
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        m_preset.name = m_nameEdit->text().toStdString();
        m_pipeline->updateConvPreset(m_preset);
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addStretch();

    auto delBtn = new QPushButton("🗑 Delete", headerWidget);
    delBtn->setStyleSheet("QPushButton { color: #ff3b30; font-weight: bold; }");
    connect(delBtn, &QPushButton::clicked, this, &ConvolutionPresetDetailView::onDeleteClicked);
    headerLayout->addWidget(delBtn);

    outerLayout->addWidget(headerWidget);

    auto headerDivider = new QFrame(this);
    headerDivider->setFrameShape(QFrame::HLine);
    headerDivider->setFrameShadow(QFrame::Sunken);
    outerLayout->addWidget(headerDivider);

    // Scroll View
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    container->setStyleSheet(QString("background-color: %1;").arg(StyleTheme::cardBg().name()));
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // Details Group
    auto detailsGroup = new QGroupBox("Details", container);
    auto detailsLayout = new QVBoxLayout(detailsGroup);
    detailsLayout->setContentsMargins(12, 16, 12, 12);
    detailsLayout->setSpacing(6);

    auto addRow = [container, detailsLayout](const QString& key, QLabel*& valueLbl) {
        auto row = new QHBoxLayout();
        auto keyLbl = new QLabel(key, container);
        keyLbl->setFixedWidth(130);
        keyLbl->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
        row->addWidget(keyLbl);

        valueLbl = new QLabel(container);
        valueLbl->setFont(QFont("monospace", 13));
        row->addWidget(valueLbl);
        row->addStretch();
        detailsLayout->addLayout(row);
        return keyLbl;
    };

    addRow("Kind", m_kindLabel);
    addRow("Taps", m_tapsLabel);
    addRow("Rates", m_ratesLabel);
    m_latencyKeyLabel = addRow("Latency @ 48k", m_latencyValueLabel);

    mainLayout->addWidget(detailsGroup);

    // Impulse Response Group
    auto irGroup = new QGroupBox("Impulse Response", container);
    auto irLayout = new QVBoxLayout(irGroup);
    irLayout->setContentsMargins(12, 16, 12, 12);
    irLayout->setSpacing(8);

    m_rateBoxWidget = new QWidget(irGroup);
    auto rateBox = new QHBoxLayout(m_rateBoxWidget);
    rateBox->setContentsMargins(0, 0, 0, 0);

    auto prevLbl = new QLabel("Preview rate", m_rateBoxWidget);
    prevLbl->setStyleSheet(QString("color: %1; font-size: 11px;").arg(StyleTheme::textSecondary().name()));
    rateBox->addWidget(prevLbl);

    m_ratePreviewCombo = new QComboBox(m_rateBoxWidget);
    m_ratePreviewCombo->setMaximumWidth(160);
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
        m_latencyKeyLabel->setText(QString("Latency @ %1k").arg(m_previewRate / 1000));
        m_latencyValueLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
    });
    rateBox->addWidget(m_ratePreviewCombo);
    rateBox->addStretch();

    irLayout->addWidget(m_rateBoxWidget);

    m_irPlot = new ConvolutionIRPlot(irGroup);
    m_irPlot->setFixedHeight(180);
    irLayout->addWidget(m_irPlot);

    m_noIrLabel = new QLabel(irGroup);
    m_noIrLabel->setStyleSheet(
        QString("color: %1; font-size: 13px; padding: 8px;").arg(StyleTheme::textSecondary().name()));
    m_noIrLabel->setVisible(false);
    irLayout->addWidget(m_noIrLabel);

    mainLayout->addWidget(irGroup);

    // Files Group
    auto filesGroup = new QGroupBox("Files", container);
    auto filesLayout = new QVBoxLayout(filesGroup);
    filesLayout->setContentsMargins(12, 16, 12, 12);
    filesLayout->setSpacing(6);

    m_filesContainer = new QWidget(filesGroup);
    m_filesContainer->setLayout(new QVBoxLayout());
    m_filesContainer->layout()->setContentsMargins(0, 0, 0, 0);
    m_filesContainer->layout()->setSpacing(6);
    filesLayout->addWidget(m_filesContainer);

    mainLayout->addWidget(filesGroup);
    mainLayout->addStretch();

    scroll->setWidget(container);
    outerLayout->addWidget(scroll);
}

void ConvolutionPresetDetailView::refreshUi() {
    m_nameEdit->setText(QString::fromStdString(m_preset.name));
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
        if (comboIdx >= 0)
            m_ratePreviewCombo->setCurrentIndex(comboIdx);

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
        m_latencyKeyLabel->setText(QString("Latency @ %1k").arg(m_previewRate / 1000));
        m_latencyValueLabel->setText(ms > 0 ? QString("%1 ms").arg(ms, 0, 'f', 1) : "≈ 0 ms (min-phase)");
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
            fileRow->setContentsMargins(0, 0, 0, 0);
            fileRow->setSpacing(8);

            auto rateLbl = new QLabel(QString("%1 Hz").arg(r), m_filesContainer);
            rateLbl->setFixedWidth(80);
            rateLbl->setFont(QFont("monospace", 11));
            rateLbl->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
            fileRow->addWidget(rateLbl);

            auto pathLbl = new QLabel(m_filesContainer);
            pathLbl->setFont(QFont("monospace", 11));
            pathLbl->setText(QFontMetrics(pathLbl->font()).elidedText(p, Qt::ElideMiddle, 280));
            pathLbl->setToolTip(p);
            fileRow->addWidget(pathLbl, 1);

            auto openBtn = new QPushButton("📁", m_filesContainer);
            openBtn->setFlat(true);
            openBtn->setToolTip("Reveal in Finder");
            openBtn->setStyleSheet(QString("QPushButton { border: none; background: transparent; color: %1; }")
                                       .arg(StyleTheme::textSecondary().name()));
            connect(openBtn, &QPushButton::clicked, [p]() {
#ifdef Q_OS_MAC
                QProcess::execute("/usr/bin/open", QStringList() << "-R" << p);
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
#endif
            });
            fileRow->addWidget(openBtn);

            filesLayout->addLayout(fileRow);
        }
    }
}

void ConvolutionPresetDetailView::onDeleteClicked() {
    m_pipeline->deleteConvPreset(m_preset.id);
}
