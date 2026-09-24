#include "ui/GeneralSettingsView.h"

#include <QCheckBox>     // for QCheckBox
#include <QComboBox>     // for QComboBox
#include <QFont>         // for QFont
#include <QFontDatabase> // for QFontDatabase
#include <QFormLayout>   // for QFormLayout
#include <QGroupBox>     // for QGroupBox
#include <QHBoxLayout>   // for QHBoxLayout
#include <QLabel>        // for QLabel
#include <QScrollArea>   // for QScrollArea
#include <QSlider>       // for QSlider
#include <QSpinBox>      // for QSpinBox
#include <QString>       // for QString
#include <QTimer>        // for QTimer
#include <QVBoxLayout>   // for QVBoxLayout
#include <Qt>            // for AlignmentFlag, Orientation, operator|

static void synchronizeFormLabels(const std::vector<QFormLayout*>& forms) {
    int maxW = 0;
    QList<QLabel*> labels;
    for (auto* form : forms) {
        if (!form)
            continue;
        for (int i = 0; i < form->rowCount(); ++i) {
            auto* item = form->itemAt(i, QFormLayout::LabelRole);
            if (item && item->widget()) {
                if (auto* lbl = qobject_cast<QLabel*>(item->widget())) {
                    labels.append(lbl);
                    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                    int w = lbl->fontMetrics().horizontalAdvance(lbl->text());
                    if (w > maxW)
                        maxW = w;
                }
            }
        }
    }
    for (auto* lbl : labels) {
        lbl->setFixedWidth(maxW);
    }
}

GeneralSettingsView::GeneralSettingsView(std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<AudioDeviceManager> devices,
                                         std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_devices(devices), m_monitoring(monitoring) {
    setupUi();
    refreshUi();

    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, &GeneralSettingsView::refreshUi);
        connect(m_settings.get(), &AudioSettings::changed, this, &GeneralSettingsView::refreshUi);
    }
    if (m_devices) {
        connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &GeneralSettingsView::updateLatencyText);
    }
}

void GeneralSettingsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshUi();
}

void GeneralSettingsView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    const QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // 1. Processing Group
    auto procGroup = new QGroupBox(tr("Processing"), container);
    m_procForm = new QFormLayout(procGroup);
    m_procForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto chunkLayout = new QHBoxLayout();
    m_chunkSizeCombo = new QComboBox(procGroup);
    for (int size : {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768}) {
        m_chunkSizeCombo->addItem(QString("%1 samples").arg(size), size);
    }
    chunkLayout->addWidget(m_chunkSizeCombo);

    m_latencyLabel = new QLabel(procGroup);
    connect(m_chunkSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() {
            if (m_chunkSizeCombo && m_settings) {
                m_settings->chunkSize = m_chunkSizeCombo->currentData().toInt();
            }
            if (m_targetLevelSpin && m_settings) {
                m_targetLevelSpin->setSpecialValueText(QString(tr("Auto (%1 samples)")).arg(m_settings->chunkSize));
            }
            updateLatencyText();
            applySettings();
        });
    });
    chunkLayout->addWidget(m_latencyLabel);
    chunkLayout->addStretch();
    m_procForm->addRow(tr("Chunk Size:"), chunkLayout);

    m_latencyEquationLabel = new QLabel(procGroup);
    QFont eqFont = m_latencyEquationLabel->font();
    eqFont.setPointSize(eqFont.pointSize() > 2 ? eqFont.pointSize() - 1 : 10);
    m_latencyEquationLabel->setFont(eqFont);
    m_latencyEquationLabel->setWordWrap(true);
    m_latencyEquationLabel->setForegroundRole(QPalette::PlaceholderText);
    m_procForm->addRow(m_latencyEquationLabel);

    m_targetLevelSpin = new QSpinBox(procGroup);
    m_targetLevelSpin->setRange(0, 1048576);
    m_targetLevelSpin->setSingleStep(64);
    m_targetLevelSpin->setSpecialValueText(tr("Auto (match chunk size)"));
    m_targetLevelSpin->setSuffix(tr(" samples"));
    connect(m_targetLevelSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        if (m_isRefreshing)
            return;
        if (m_settings) {
            m_settings->targetLevel = val;
        }
        updateLatencyText();
        applySettings();
    });
    m_procForm->addRow(tr("Target Level:"), m_targetLevelSpin);

    m_targetLevelSub = new QLabel(tr("Playback buffer target level in samples. Default matches chunk size. Regulated "
                                     "by rate adjust to prevent underruns."),
                                  procGroup);
    {
        QFont font = m_targetLevelSub->font();
        font.setPointSize(11);
        m_targetLevelSub->setFont(font);
        m_targetLevelSub->setObjectName("secondaryLabel");
    }
    m_targetLevelSub->setWordWrap(true);
    m_procForm->addRow(m_targetLevelSub);

    m_enableRateAdjustCheck = new QCheckBox(tr("Enable Rate Adjust"), procGroup);
    connect(m_enableRateAdjustCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        if (m_procForm && m_rateAdjustIntervalRow) {
            m_procForm->setRowVisible(m_rateAdjustIntervalRow, checked);
        }
        if (m_settings) {
            m_settings->enableRateAdjust = checked;
        }
        applySettings();
    });
    m_procForm->addRow(m_enableRateAdjustCheck);

    m_rateAdjustSub = new QLabel(tr("Compensate for clock drift between capture and playback devices"), procGroup);
    {
        QFont font = m_rateAdjustSub->font();
        font.setPointSize(11);
        m_rateAdjustSub->setFont(font);
        m_rateAdjustSub->setObjectName("secondaryLabel");
    }
    m_rateAdjustSub->setWordWrap(true);
    m_procForm->addRow(m_rateAdjustSub);

    m_rateAdjustIntervalRow = new QWidget(procGroup);
    auto rateAdjustIntervalBox = new QHBoxLayout(m_rateAdjustIntervalRow);
    rateAdjustIntervalBox->setContentsMargins(0, 0, 0, 0);
    m_rateAdjustIntervalSlider = new QSlider(Qt::Horizontal, m_rateAdjustIntervalRow);
    m_rateAdjustIntervalSlider->setRange(5, 300); // 0.5 to 30.0 s

    m_rateAdjustIntervalValLabel = new QLabel(m_rateAdjustIntervalRow);
    m_rateAdjustIntervalValLabel->setFont(monoFont);
    m_rateAdjustIntervalValLabel->setMinimumWidth(50);

    connect(m_rateAdjustIntervalSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        double dVal = val / 10.0;
        m_rateAdjustIntervalValLabel->setText(QString("%1 s").arg(dVal, 0, 'f', 1));
        if (m_settings) {
            m_settings->rateAdjustInterval = dVal;
        }
        applySettings();
    });
    rateAdjustIntervalBox->addWidget(m_rateAdjustIntervalSlider);
    rateAdjustIntervalBox->addWidget(m_rateAdjustIntervalValLabel);
    rateAdjustIntervalBox->addStretch();
    m_procForm->addRow(tr("Rate Adjust Interval:"), m_rateAdjustIntervalRow);

    auto intervalBox = new QHBoxLayout();
    m_measureIntervalSlider = new QSlider(Qt::Horizontal, procGroup);
    m_measureIntervalSlider->setRange(1, 100); // 0.1 to 10.0 s

    m_measureIntervalValLabel = new QLabel(procGroup);
    m_measureIntervalValLabel->setFont(monoFont);
    m_measureIntervalValLabel->setMinimumWidth(50);

    connect(m_measureIntervalSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        double dVal = val / 10.0;
        m_measureIntervalValLabel->setText(QString("%1 s").arg(dVal, 0, 'f', 1));
        if (m_settings) {
            m_settings->rateMeasureInterval = dVal;
        }
        applySettings();
    });
    intervalBox->addWidget(m_measureIntervalSlider);
    intervalBox->addWidget(m_measureIntervalValLabel);
    intervalBox->addStretch();
    m_procForm->addRow(tr("Rate Measure Interval:"), intervalBox);

    m_queueLimitSpin = new QSpinBox(procGroup);
    m_queueLimitSpin->setRange(1, 32);
    connect(m_queueLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        if (m_isRefreshing)
            return;
        if (m_settings)
            m_settings->queuelimit = val;
        applySettings();
        updateLatencyText();
    });
    m_procForm->addRow(tr("Queue Limit:"), m_queueLimitSpin);

    m_stopOnRateChangeCheck = new QCheckBox(tr("Stop on Rate Change"), procGroup);
    connect(m_stopOnRateChangeCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        if (m_settings) {
            m_settings->stopOnRateChange = checked;
        }
        applySettings();
    });
    m_procForm->addRow(m_stopOnRateChangeCheck);

    m_multithreadedCheck = new QCheckBox(tr("Multithreaded"), procGroup);
    connect(m_multithreadedCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        if (m_procForm && m_workerThreadsSpin) {
            m_procForm->setRowVisible(m_workerThreadsSpin, checked);
        }
        if (m_settings) {
            m_settings->multithreaded = checked;
        }
        applySettings();
    });
    m_procForm->addRow(m_multithreadedCheck);

    m_workerThreadsSpin = new QSpinBox(procGroup);
    m_workerThreadsSpin->setRange(0, 32);
    m_workerThreadsSpin->setSpecialValueText(tr("Auto"));
    connect(m_workerThreadsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int val) {
        if (m_isRefreshing)
            return;
        if (m_settings) {
            m_settings->workerThreads = val;
        }
        applySettings();
    });
    m_procForm->addRow(tr("Worker Threads:"), m_workerThreadsSpin);

    mainLayout->addWidget(procGroup);

    // 2. Polling Rate Group
    auto pollGroup = new QGroupBox(tr("Polling Rate"), container);
    auto pollForm = new QFormLayout(pollGroup);
    pollForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto pollSliderLayout = new QHBoxLayout();
    m_pollingRateSlider = new QSlider(Qt::Horizontal, pollGroup);
    m_pollingRateSlider->setRange(1, 60);
    pollSliderLayout->addWidget(m_pollingRateSlider);

    m_pollingRateLabel = new QLabel("30 Hz", pollGroup);
    m_pollingRateLabel->setFont(monoFont);
    m_pollingRateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pollingRateLabel->setMinimumWidth(60);
    pollSliderLayout->addWidget(m_pollingRateLabel);

    pollForm->addRow(tr("Polling Rate:"), pollSliderLayout);

    auto pollSubLbl = new QLabel(tr("Adjust the frequency of UI updates for meters and spectrum."), pollGroup);
    pollSubLbl->setWordWrap(true);
    {
        QFont font = pollSubLbl->font();
        font.setPointSize(11);
        pollSubLbl->setFont(font);
        pollSubLbl->setObjectName("secondaryLabel");
    }
    pollForm->addRow(pollSubLbl);

    connect(m_pollingRateSlider, &QSlider::valueChanged, [this](int val) {
        if (m_monitoring) {
            m_monitoring->setPollingRate(static_cast<double>(val));
        }
        m_pollingRateLabel->setText(QString("%1 Hz").arg(val));
        if (m_settings) {
            m_settings->savePreferences();
        }
    });

    mainLayout->addWidget(pollGroup);

    // 3. Silence Detection Group
    auto silenceGroup = new QGroupBox(tr("Silence Detection"), container);
    auto silenceForm = new QFormLayout(silenceGroup);
    silenceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto threshSliderLayout = new QHBoxLayout();
    m_silenceThresholdSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceThresholdSlider->setRange(-120, 0);
    threshSliderLayout->addWidget(m_silenceThresholdSlider);

    m_silenceThresholdLabel = new QLabel("-60 dB", silenceGroup);
    m_silenceThresholdLabel->setFont(monoFont);
    m_silenceThresholdLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceThresholdLabel->setMinimumWidth(60);
    threshSliderLayout->addWidget(m_silenceThresholdLabel);

    silenceForm->addRow(tr("Silence Threshold:"), threshSliderLayout);

    connect(m_silenceThresholdSlider, &QSlider::valueChanged, [this](int val) {
        if (m_settings) {
            m_settings->setSilenceThreshold(val);
        }
        m_silenceThresholdLabel->setText(QString("%1 dB").arg(val));
    });

    auto timeoutSliderLayout = new QHBoxLayout();
    m_silenceTimeoutSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceTimeoutSlider->setRange(0, 60);
    timeoutSliderLayout->addWidget(m_silenceTimeoutSlider);

    m_silenceTimeoutLabel = new QLabel(tr("Disabled"), silenceGroup);
    m_silenceTimeoutLabel->setFont(monoFont);
    m_silenceTimeoutLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceTimeoutLabel->setMinimumWidth(60);
    timeoutSliderLayout->addWidget(m_silenceTimeoutLabel);

    silenceForm->addRow(tr("Silence Timeout:"), timeoutSliderLayout);

    connect(m_silenceTimeoutSlider, &QSlider::valueChanged, [this](int val) {
        if (m_settings) {
            m_settings->setSilenceTimeout(val);
        }
        if (val == 0) {
            m_silenceTimeoutLabel->setText(tr("Disabled"));
        } else {
            m_silenceTimeoutLabel->setText(QString("%1 s").arg(val));
        }
    });

    auto silenceSubLbl =
        new QLabel(tr("Pause processing if the input signal is silent for the specified duration."), silenceGroup);
    silenceSubLbl->setWordWrap(true);
    {
        QFont font = silenceSubLbl->font();
        font.setPointSize(11);
        silenceSubLbl->setFont(font);
        silenceSubLbl->setObjectName("secondaryLabel");
    }
    silenceForm->addRow(silenceSubLbl);

    mainLayout->addWidget(silenceGroup);
    mainLayout->addStretch();

    scroll->setWidget(container);

    synchronizeFormLabels({m_procForm, pollForm, silenceForm});

    auto outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(scroll);
}

void GeneralSettingsView::applySettings() {
    if (m_isRefreshing)
        return;
    if (m_settings) {
        if (m_chunkSizeCombo)
            m_settings->chunkSize = m_chunkSizeCombo->currentData().toInt();
        if (m_targetLevelSpin)
            m_settings->targetLevel = m_targetLevelSpin->value();
        if (m_enableRateAdjustCheck)
            m_settings->enableRateAdjust = m_enableRateAdjustCheck->isChecked();
        if (m_rateAdjustIntervalSlider)
            m_settings->rateAdjustInterval = m_rateAdjustIntervalSlider->value() / 10.0;
        if (m_queueLimitSpin)
            m_settings->queuelimit = m_queueLimitSpin->value();
        if (m_stopOnRateChangeCheck)
            m_settings->stopOnRateChange = m_stopOnRateChangeCheck->isChecked();
        if (m_measureIntervalSlider)
            m_settings->rateMeasureInterval = m_measureIntervalSlider->value() / 10.0;
        if (m_multithreadedCheck)
            m_settings->multithreaded = m_multithreadedCheck->isChecked();
        if (m_workerThreadsSpin)
            m_settings->workerThreads = m_workerThreadsSpin->value();
        m_settings->savePreferences();
    }
}

void GeneralSettingsView::refreshUi() {
    m_isRefreshing = true;

    if (m_settings) {
        // Processing settings
        if (m_chunkSizeCombo) {
            m_chunkSizeCombo->blockSignals(true);
            int chunkIdx = m_chunkSizeCombo->findData(m_settings->chunkSize);
            if (chunkIdx >= 0)
                m_chunkSizeCombo->setCurrentIndex(chunkIdx);
            m_chunkSizeCombo->blockSignals(false);
        }

        if (m_targetLevelSpin) {
            m_targetLevelSpin->blockSignals(true);
            m_targetLevelSpin->setSpecialValueText(QString(tr("Auto (%1 samples)")).arg(m_settings->chunkSize));
            m_targetLevelSpin->setValue(m_settings->targetLevel);
            m_targetLevelSpin->blockSignals(false);
        }

        if (m_enableRateAdjustCheck) {
            m_enableRateAdjustCheck->blockSignals(true);
            m_enableRateAdjustCheck->setChecked(m_settings->enableRateAdjust);
            m_enableRateAdjustCheck->blockSignals(false);
        }

        if (m_rateAdjustIntervalSlider && !m_rateAdjustIntervalSlider->isSliderDown()) {
            m_rateAdjustIntervalSlider->blockSignals(true);
            m_rateAdjustIntervalSlider->setValue(static_cast<int>(m_settings->rateAdjustInterval * 10.0));
            m_rateAdjustIntervalSlider->blockSignals(false);
        }
        if (m_rateAdjustIntervalValLabel) {
            m_rateAdjustIntervalValLabel->setText(QString("%1 s").arg(m_settings->rateAdjustInterval, 0, 'f', 1));
        }
        if (m_procForm && m_rateAdjustIntervalRow) {
            m_procForm->setRowVisible(m_rateAdjustIntervalRow, m_settings->enableRateAdjust);
        }

        if (m_queueLimitSpin) {
            m_queueLimitSpin->blockSignals(true);
            m_queueLimitSpin->setValue(m_settings->queuelimit);
            m_queueLimitSpin->blockSignals(false);
        }

        if (m_stopOnRateChangeCheck) {
            m_stopOnRateChangeCheck->blockSignals(true);
            m_stopOnRateChangeCheck->setChecked(m_settings->stopOnRateChange);
            m_stopOnRateChangeCheck->blockSignals(false);
        }

        if (m_measureIntervalSlider && !m_measureIntervalSlider->isSliderDown()) {
            m_measureIntervalSlider->blockSignals(true);
            m_measureIntervalSlider->setValue(static_cast<int>(m_settings->rateMeasureInterval * 10.0));
            m_measureIntervalSlider->blockSignals(false);
        }
        if (m_measureIntervalValLabel) {
            m_measureIntervalValLabel->setText(QString("%1 s").arg(m_settings->rateMeasureInterval, 0, 'f', 1));
        }

        if (m_multithreadedCheck) {
            m_multithreadedCheck->blockSignals(true);
            m_multithreadedCheck->setChecked(m_settings->multithreaded);
            m_multithreadedCheck->blockSignals(false);
        }
        if (m_procForm && m_workerThreadsSpin) {
            m_procForm->setRowVisible(m_workerThreadsSpin, m_settings->multithreaded);
        }
        if (m_workerThreadsSpin) {
            m_workerThreadsSpin->blockSignals(true);
            m_workerThreadsSpin->setValue(m_settings->workerThreads);
            m_workerThreadsSpin->blockSignals(false);
        }

        // Silence settings
        if (m_silenceThresholdSlider && !m_silenceThresholdSlider->isSliderDown()) {
            m_silenceThresholdSlider->blockSignals(true);
            m_silenceThresholdSlider->setValue(m_settings->silenceThreshold);
            m_silenceThresholdSlider->blockSignals(false);
        }
        if (m_silenceThresholdLabel) {
            m_silenceThresholdLabel->setText(QString("%1 dB").arg(m_settings->silenceThreshold));
        }

        if (m_silenceTimeoutSlider && !m_silenceTimeoutSlider->isSliderDown()) {
            m_silenceTimeoutSlider->blockSignals(true);
            m_silenceTimeoutSlider->setValue(m_settings->silenceTimeout);
            m_silenceTimeoutSlider->blockSignals(false);
        }
        if (m_silenceTimeoutLabel) {
            if (m_settings->silenceTimeout == 0) {
                m_silenceTimeoutLabel->setText(tr("Disabled"));
            } else {
                m_silenceTimeoutLabel->setText(QString("%1 s").arg(m_settings->silenceTimeout));
            }
        }
    }

    updateLatencyText();

    if (m_monitoring) {
        int pollRate = static_cast<int>(m_monitoring->pollingRate());
        if (m_pollingRateSlider && !m_pollingRateSlider->isSliderDown()) {
            m_pollingRateSlider->blockSignals(true);
            m_pollingRateSlider->setValue(pollRate);
            m_pollingRateSlider->blockSignals(false);
        }
        if (m_pollingRateLabel) {
            m_pollingRateLabel->setText(QString("%1 Hz").arg(pollRate));
        }
    }

    m_isRefreshing = false;
}

void GeneralSettingsView::updateLatencyText() {
    if (!m_latencyLabel || !m_devices)
        return;

    auto info = m_devices->latencyInfo();
    m_latencyLabel->setText(QString(tr("(~%1 ms nominal | bounds: %2 – %3 ms)"))
                                .arg(info.nominalMs, 0, 'f', 1)
                                .arg(info.minMs, 0, 'f', 1)
                                .arg(info.maxMs, 0, 'f', 1));

    QString tooltip =
        QString(
            tr("<b>End-to-End Latency Breakdown:</b><br>"
               "• <b>Capture Buffer:</b> %1 samples @ %2 Hz = <b>%3 ms</b> (1 chunk accumulation)<br>"
               "• <b>Inter-Thread Queues:</b> %4 chunk(s) limit per queue<br>"
               "&nbsp;&nbsp;- Nominal steady-state in-flight: 1 chunk = <b>%5 ms</b><br>"
               "&nbsp;&nbsp;- Max queue backlog: 2 × %4 = %6 chunks = <b>%7 ms</b><br>"
               "• <b>Playback Target Level:</b> %8 samples @ %9 Hz = <b>%10 ms</b>%11<br>"
               "%12<br>"
               "<b>Equations:</b><br>"
               "• <b>Nominal:</b> <code>(chunk / fs_cap) + (chunk + target_level) / fs_pb%13</code> = <b>%14 ms</b><br>"
               "• <b>Lower Bound (Min):</b> <code>(chunk / fs_cap) + (target_level / fs_pb)%13</code> = <b>%15 "
               "ms</b><br>"
               "• <b>Upper Bound (Max):</b> <code>(chunk / fs_cap) + ((2 × queuelimit × chunk + target_level) / "
               "fs_pb)%13</code> = <b>%16 ms</b>"))
            .arg(info.chunkSize)
            .arg(info.captureRate)
            .arg(info.captureMs, 0, 'f', 1)
            .arg(info.queueLimit)
            .arg(info.queueNominalMs, 0, 'f', 1)
            .arg(2 * info.queueLimit)
            .arg(info.queueMaxMs, 0, 'f', 1)
            .arg(info.targetLevel)
            .arg(info.playbackRate)
            .arg(info.playbackTargetMs, 0, 'f', 1)
            .arg(m_settings && m_settings->targetLevel == 0 ? tr(" <i>(auto = chunk size)</i>") : "")
            .arg(info.hasResampler ? QString(tr("• <b>Resampler Filter Delay:</b> <b>%1 ms</b><br>"))
                                         .arg(info.resamplerDelayMs, 0, 'f', 1)
                                   : "")
            .arg(info.hasResampler ? tr(" + resampler_delay") : "")
            .arg(info.nominalMs, 0, 'f', 1)
            .arg(info.minMs, 0, 'f', 1)
            .arg(info.maxMs, 0, 'f', 1);

    m_latencyLabel->setToolTip(tooltip);

    if (m_latencyEquationLabel) {
        if (info.hasResampler) {
            m_latencyEquationLabel->setText(
                tr("Latency Eq: Nominal ≈ (chunk/fs_cap) + (chunk + target_level)/fs_pb + filter_delay "
                   "| Bounds: [%1 ms – %2 ms]")
                    .arg(info.minMs, 0, 'f', 1)
                    .arg(info.maxMs, 0, 'f', 1));
        } else {
            m_latencyEquationLabel->setText(tr("Latency Eq: Nominal ≈ (2·chunk + target_level)/fs "
                                               "| Bounds: [%1 ms – %2 ms]")
                                                .arg(info.minMs, 0, 'f', 1)
                                                .arg(info.maxMs, 0, 'f', 1));
        }
        m_latencyEquationLabel->setToolTip(tooltip);
    }
}
