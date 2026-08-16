#include "ui/GeneralSettingsView.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

GeneralSettingsView::GeneralSettingsView(std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<MonitoringController> monitoring, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_monitoring(monitoring) {
    setupUi();
    refreshUi();

    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, &GeneralSettingsView::refreshUi);
        connect(m_settings.get(), &AudioSettings::changed, this, &GeneralSettingsView::refreshUi);
    }
}

void GeneralSettingsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshUi();
}

void GeneralSettingsView::setupUi() {
    setMinimumWidth(450);
    setMaximumWidth(650);

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // Appearance Group (Theme Switching: Light / Dark)
    auto appGroup = new QGroupBox("Appearance", this);
    auto appForm = new QFormLayout(appGroup);

    auto themeBox = new QHBoxLayout();
    auto themeTitleLabel = new QLabel("Theme Mode", appGroup);
    themeTitleLabel->setFixedWidth(120);
    themeBox->addWidget(themeTitleLabel);

    m_lightThemeRadio = new QRadioButton("Light Mode", appGroup);
    m_darkThemeRadio = new QRadioButton("Dark Mode", appGroup);

    themeBox->addWidget(m_lightThemeRadio);
    themeBox->addWidget(m_darkThemeRadio);
    themeBox->addStretch();

    appForm->addRow(themeBox);

    auto themeBtnGroup = new QButtonGroup(this);
    themeBtnGroup->addButton(m_lightThemeRadio, 0);
    themeBtnGroup->addButton(m_darkThemeRadio, 1);

    connect(themeBtnGroup, &QButtonGroup::idClicked, [this](int id) {
        if (m_settings) {
            bool isDark = (id == 1);
            if (m_settings->darkMode != isDark) {
                m_settings->darkMode = isDark;
                m_settings->savePreferences();
                emit m_settings->settingsChanged();
            }
        }
    });

    mainLayout->addWidget(appGroup);

    // Polling Rate Group
    auto pollGroup = new QGroupBox("Polling Rate", this);
    auto pollForm = new QFormLayout(pollGroup);

    auto pollBox = new QHBoxLayout();
    auto pollTitleLabel = new QLabel("Polling Rate", pollGroup);
    pollTitleLabel->setFixedWidth(120);
    pollBox->addWidget(pollTitleLabel);

    m_pollingRateSlider = new QSlider(Qt::Horizontal, pollGroup);
    m_pollingRateSlider->setRange(1, 60);
    pollBox->addWidget(m_pollingRateSlider);

    m_pollingRateLabel = new QLabel("30 Hz", pollGroup);
    m_pollingRateLabel->setFont(QFont("monospace", 11));
    m_pollingRateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pollingRateLabel->setMinimumWidth(70);
    pollBox->addWidget(m_pollingRateLabel);

    pollForm->addRow(pollBox);

    auto pollSubLbl = new QLabel("Adjust the frequency of UI updates for meters and spectrum.", pollGroup);
    pollSubLbl->setProperty("secondary", true);
    {
        QFont font = pollSubLbl->font();
        font.setPointSize(11);
        pollSubLbl->setFont(font);
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

    // Silence Detection Group
    auto silenceGroup = new QGroupBox("Silence Detection", this);
    auto silenceForm = new QFormLayout(silenceGroup);

    auto threshBox = new QHBoxLayout();
    auto threshTitleLabel = new QLabel("Silence Threshold", silenceGroup);
    threshTitleLabel->setFixedWidth(120);
    threshBox->addWidget(threshTitleLabel);

    m_silenceThresholdSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceThresholdSlider->setRange(-120, 0);
    threshBox->addWidget(m_silenceThresholdSlider);

    m_silenceThresholdLabel = new QLabel("-60 dB", silenceGroup);
    m_silenceThresholdLabel->setFont(QFont("monospace", 11));
    m_silenceThresholdLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceThresholdLabel->setFixedWidth(60);
    threshBox->addWidget(m_silenceThresholdLabel);

    silenceForm->addRow(threshBox);

    connect(m_silenceThresholdSlider, &QSlider::valueChanged, [this](int val) {
        if (m_settings) {
            m_settings->setSilenceThreshold(val);
        }
        m_silenceThresholdLabel->setText(QString("%1 dB").arg(val));
    });

    auto timeoutBox = new QHBoxLayout();
    auto timeoutTitleLabel = new QLabel("Silence Timeout", silenceGroup);
    timeoutTitleLabel->setFixedWidth(120);
    timeoutBox->addWidget(timeoutTitleLabel);

    m_silenceTimeoutSlider = new QSlider(Qt::Horizontal, silenceGroup);
    m_silenceTimeoutSlider->setRange(0, 60);
    timeoutBox->addWidget(m_silenceTimeoutSlider);

    m_silenceTimeoutLabel = new QLabel("Disabled", silenceGroup);
    m_silenceTimeoutLabel->setFont(QFont("monospace", 11));
    m_silenceTimeoutLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceTimeoutLabel->setFixedWidth(60);
    timeoutBox->addWidget(m_silenceTimeoutLabel);

    silenceForm->addRow(timeoutBox);

    connect(m_silenceTimeoutSlider, &QSlider::valueChanged, [this](int val) {
        if (m_settings) {
            m_settings->setSilenceTimeout(val);
        }
        if (val == 0) {
            m_silenceTimeoutLabel->setText("Disabled");
        } else {
            m_silenceTimeoutLabel->setText(QString("%1 s").arg(val));
        }
    });

    auto silenceSubLbl =
        new QLabel("Pause processing if the input signal is silent for the specified duration.", silenceGroup);
    silenceSubLbl->setProperty("secondary", true);
    {
        QFont font = silenceSubLbl->font();
        font.setPointSize(11);
        silenceSubLbl->setFont(font);
    }
    silenceForm->addRow(silenceSubLbl);

    mainLayout->addWidget(silenceGroup);
    mainLayout->addStretch();
}

void GeneralSettingsView::refreshUi() {
    if (m_settings) {
        m_lightThemeRadio->blockSignals(true);
        m_darkThemeRadio->blockSignals(true);
        m_lightThemeRadio->setChecked(!m_settings->darkMode);
        m_darkThemeRadio->setChecked(m_settings->darkMode);
        m_lightThemeRadio->blockSignals(false);
        m_darkThemeRadio->blockSignals(false);

        if (!m_silenceThresholdSlider->isSliderDown()) {
            m_silenceThresholdSlider->blockSignals(true);
            m_silenceThresholdSlider->setValue(m_settings->silenceThreshold);
            m_silenceThresholdSlider->blockSignals(false);
        }
        m_silenceThresholdLabel->setText(QString("%1 dB").arg(m_settings->silenceThreshold));

        if (!m_silenceTimeoutSlider->isSliderDown()) {
            m_silenceTimeoutSlider->blockSignals(true);
            m_silenceTimeoutSlider->setValue(m_settings->silenceTimeout);
            m_silenceTimeoutSlider->blockSignals(false);
        }
        if (m_settings->silenceTimeout == 0) {
            m_silenceTimeoutLabel->setText("Disabled");
        } else {
            m_silenceTimeoutLabel->setText(QString("%1 s").arg(m_settings->silenceTimeout));
        }
    }

    if (m_monitoring) {
        int pollRate = static_cast<int>(m_monitoring->pollingRate());
        if (!m_pollingRateSlider->isSliderDown()) {
            m_pollingRateSlider->blockSignals(true);
            m_pollingRateSlider->setValue(pollRate);
            m_pollingRateSlider->blockSignals(false);
        }
        m_pollingRateLabel->setText(QString("%1 Hz").arg(pollRate));
    }
}
