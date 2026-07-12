#include "ui/GeneralSettingsView.h"

#include "ui/StyleTheme.h"

#include <QApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

GeneralSettingsView::GeneralSettingsView(std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_monitoring(monitoring) {
    setupUi();
    refreshUi();
}

void GeneralSettingsView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    // Theme Group
    auto themeGroup = new QGroupBox("Appearance & Theme", this);
    auto themeForm = new QFormLayout(themeGroup);

    m_themeCombo = new QComboBox(themeGroup);
    m_themeCombo->addItem("macOS Light Theme (Default)", 0);
    m_themeCombo->addItem("Dark Theme", 1);
    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_settings->darkMode = (idx == 1);
        m_settings->savePreferences();
        StyleTheme::applyTheme(qApp, m_settings->darkMode ? AppTheme::Dark : AppTheme::Light);
        for (QWidget* w : qApp->allWidgets()) {
            w->update();
        }
    });
    themeForm->addRow("UI Theme:", m_themeCombo);

    m_logLevelCombo = new QComboBox(themeGroup);
    m_logLevelCombo->addItems({"Trace & Above", "Debug & Above", "Info & Above", "Warn & Above", "Error Only"});
    connect(m_logLevelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_settings->logLevel = idx;
        m_settings->savePreferences();
    });
    themeForm->addRow("Default Log Level:", m_logLevelCombo);

    m_autoStartCheck = new QCheckBox("Auto-start DSP Engine on Launch", themeGroup);
    connect(m_autoStartCheck, &QCheckBox::toggled, [this](bool checked) {
        m_settings->autoStartEngine = checked;
        m_settings->savePreferences();
    });
    themeForm->addRow("Auto-Start Options:", m_autoStartCheck);

    mainLayout->addWidget(themeGroup);

    // Monitoring Refresh Group
    auto pollGroup = new QGroupBox("UI Refresh Rate", this);
    auto pollForm = new QFormLayout(pollGroup);

    auto pollBox = new QHBoxLayout();
    m_pollingRateSlider = new QSlider(Qt::Horizontal, pollGroup);
    m_pollingRateSlider->setRange(1, 60);
    pollBox->addWidget(m_pollingRateSlider);

    m_pollingRateLabel = new QLabel("30 Hz", pollGroup);
    m_pollingRateLabel->setFont(QFont("monospace", 11));
    m_pollingRateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pollingRateLabel->setMinimumWidth(80);
    pollBox->addWidget(m_pollingRateLabel);

    connect(m_pollingRateSlider, &QSlider::valueChanged, [this](int val) {
        m_monitoring->setPollingRate(static_cast<double>(val));
        m_pollingRateLabel->setText(QString("%1 Hz").arg(val));
        if (m_settings)
            m_settings->savePreferences();
    });
    pollForm->addRow("Monitoring Polling Rate:", pollBox);

    auto pollSubLbl = new QLabel("Adjust the frequency of UI updates for meters and spectrum.", pollGroup);
    pollSubLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    pollForm->addRow("", pollSubLbl);

    mainLayout->addWidget(pollGroup);

    // Silence Detection Group
    auto silenceGroup = new QGroupBox("Silence Detection", this);
    auto silenceForm = new QFormLayout(silenceGroup);

    auto threshBox = new QHBoxLayout();
    m_silenceThresholdSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceThresholdSlider->setRange(-120, 0);
    threshBox->addWidget(m_silenceThresholdSlider);

    m_silenceThresholdLabel = new QLabel("-60 dB", silenceGroup);
    m_silenceThresholdLabel->setFont(QFont("monospace", 11));
    m_silenceThresholdLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceThresholdLabel->setMinimumWidth(80);
    threshBox->addWidget(m_silenceThresholdLabel);

    connect(m_silenceThresholdSlider, &QSlider::valueChanged, [this](int val) {
        m_settings->silenceThreshold = val;
        m_silenceThresholdLabel->setText(QString("%1 dB").arg(val));
        m_settings->savePreferences();
    });
    silenceForm->addRow("Silence Threshold:", threshBox);

    auto timeoutBox = new QHBoxLayout();
    m_silenceTimeoutSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceTimeoutSlider->setRange(0, 60);
    timeoutBox->addWidget(m_silenceTimeoutSlider);

    m_silenceTimeoutLabel = new QLabel("Disabled", silenceGroup);
    m_silenceTimeoutLabel->setFont(QFont("monospace", 11));
    m_silenceTimeoutLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceTimeoutLabel->setMinimumWidth(85);
    timeoutBox->addWidget(m_silenceTimeoutLabel);

    connect(m_silenceTimeoutSlider, &QSlider::valueChanged, [this](int val) {
        m_settings->silenceTimeout = val;
        if (val == 0)
            m_silenceTimeoutLabel->setText("Disabled");
        else
            m_silenceTimeoutLabel->setText(QString("%1 s").arg(val));
        m_settings->savePreferences();
    });
    silenceForm->addRow("Silence Timeout:", timeoutBox);

    auto silenceSubLbl =
        new QLabel("Pause processing if the input signal is silent for the specified duration.", silenceGroup);
    silenceSubLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    silenceForm->addRow("", silenceSubLbl);

    mainLayout->addWidget(silenceGroup);
    mainLayout->addStretch();
}

void GeneralSettingsView::refreshUi() {
    m_themeCombo->blockSignals(true);
    m_themeCombo->setCurrentIndex(m_settings->darkMode ? 1 : 0);
    m_themeCombo->blockSignals(false);

    m_logLevelCombo->blockSignals(true);
    m_logLevelCombo->setCurrentIndex(m_settings->logLevel);
    m_logLevelCombo->blockSignals(false);

    m_autoStartCheck->blockSignals(true);
    m_autoStartCheck->setChecked(m_settings->autoStartEngine);
    m_autoStartCheck->blockSignals(false);

    int pollRate = static_cast<int>(m_monitoring->pollingRate());
    m_pollingRateSlider->blockSignals(true);
    m_pollingRateSlider->setValue(pollRate);
    m_pollingRateSlider->blockSignals(false);
    m_pollingRateLabel->setText(QString("%1 Hz").arg(pollRate));

    m_silenceThresholdSlider->blockSignals(true);
    m_silenceThresholdSlider->setValue(m_settings->silenceThreshold);
    m_silenceThresholdSlider->blockSignals(false);
    m_silenceThresholdLabel->setText(QString("%1 dB").arg(m_settings->silenceThreshold));

    m_silenceTimeoutSlider->blockSignals(true);
    m_silenceTimeoutSlider->setValue(m_settings->silenceTimeout);
    m_silenceTimeoutSlider->blockSignals(false);
    if (m_settings->silenceTimeout == 0)
        m_silenceTimeoutLabel->setText("Disabled");
    else
        m_silenceTimeoutLabel->setText(QString("%1 s").arg(m_settings->silenceTimeout));
}
