#include "ui/GeneralSettingsView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QApplication>

GeneralSettingsView::GeneralSettingsView(
    std::shared_ptr<AudioSettings> settings,
    std::shared_ptr<MonitoringController> monitoring,
    QWidget* parent
) : QWidget(parent), m_settings(settings), m_monitoring(monitoring) {
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
        if (idx == 0) {
            qApp->setStyleSheet(StyleTheme::lightStylesheet());
        } else {
            qApp->setStyleSheet(StyleTheme::darkStylesheet());
        }
    });
    themeForm->addRow("UI Theme:", m_themeCombo);
    mainLayout->addWidget(themeGroup);

    // Monitoring Refresh Group
    auto pollGroup = new QGroupBox("UI Refresh Rate", this);
    auto pollForm = new QFormLayout(pollGroup);

    auto pollBox = new QHBoxLayout();
    m_pollingRateSlider = new QSlider(Qt::Horizontal, pollGroup);
    m_pollingRateSlider->setRange(1, 60);
    pollBox->addWidget(m_pollingRateSlider);

    m_pollingRateLabel = new QLabel("30 Hz", pollGroup);
    m_pollingRateLabel->setFixedWidth(60);
    pollBox->addWidget(m_pollingRateLabel);

    connect(m_pollingRateSlider, &QSlider::valueChanged, [this](int val) {
        m_monitoring->setPollingRate(static_cast<double>(val));
        m_pollingRateLabel->setText(QString("%1 Hz").arg(val));
    });
    pollForm->addRow("Monitoring Polling Rate:", pollBox);

    mainLayout->addWidget(pollGroup);

    // Silence Detection Group
    auto silenceGroup = new QGroupBox("Silence Detection", this);
    auto silenceForm = new QFormLayout(silenceGroup);

    auto threshBox = new QHBoxLayout();
    m_silenceThresholdSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceThresholdSlider->setRange(-120, 0);
    threshBox->addWidget(m_silenceThresholdSlider);

    m_silenceThresholdLabel = new QLabel("-60 dB", silenceGroup);
    m_silenceThresholdLabel->setFixedWidth(60);
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
    m_silenceTimeoutLabel->setFixedWidth(60);
    timeoutBox->addWidget(m_silenceTimeoutLabel);

    connect(m_silenceTimeoutSlider, &QSlider::valueChanged, [this](int val) {
        m_settings->silenceTimeout = val;
        if (val == 0) m_silenceTimeoutLabel->setText("Disabled");
        else m_silenceTimeoutLabel->setText(QString("%1 s").arg(val));
        m_settings->savePreferences();
    });
    silenceForm->addRow("Silence Timeout:", timeoutBox);

    mainLayout->addWidget(silenceGroup);
    mainLayout->addStretch();
}

void GeneralSettingsView::refreshUi() {
    int pollRate = static_cast<int>(m_monitoring->pollingRate());
    m_pollingRateSlider->setValue(pollRate);
    m_pollingRateLabel->setText(QString("%1 Hz").arg(pollRate));

    m_silenceThresholdSlider->setValue(m_settings->silenceThreshold);
    m_silenceThresholdLabel->setText(QString("%1 dB").arg(m_settings->silenceThreshold));

    m_silenceTimeoutSlider->setValue(m_settings->silenceTimeout);
    if (m_settings->silenceTimeout == 0) m_silenceTimeoutLabel->setText("Disabled");
    else m_silenceTimeoutLabel->setText(QString("%1 s").arg(m_settings->silenceTimeout));
}
