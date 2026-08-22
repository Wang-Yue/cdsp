#include "ui/GeneralSettingsView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "utils/ThemeManager.h"

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
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    const QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // Appearance Group
    auto themeGroup = new QGroupBox("Appearance", this);
    auto themeForm = new QFormLayout(themeGroup);
    themeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_themeCombo = new QComboBox(themeGroup);
    m_themeCombo->addItem("System Default", static_cast<int>(AppTheme::System));
    m_themeCombo->addItem("Light", static_cast<int>(AppTheme::Light));
    m_themeCombo->addItem("Dark", static_cast<int>(AppTheme::Dark));
    m_themeCombo->setMinimumWidth(160);

    themeForm->addRow("Theme:", m_themeCombo);

    auto themeSubLbl =
        new QLabel("Select whether the app follows the system color scheme or uses a fixed light/dark theme.", themeGroup);
    themeSubLbl->setWordWrap(true);
    {
        QFont font = themeSubLbl->font();
        font.setPointSize(11);
        themeSubLbl->setFont(font);
        QPalette pal = themeSubLbl->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        themeSubLbl->setPalette(pal);
    }
    themeForm->addRow(themeSubLbl);

    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (m_settings) {
            auto newTheme = static_cast<AppTheme>(m_themeCombo->itemData(idx).toInt());
            m_settings->appTheme = newTheme;
            m_settings->savePreferences();
            ThemeManager::setTheme(newTheme);
        }
    });

    mainLayout->addWidget(themeGroup);

    // Polling Rate Group
    auto pollGroup = new QGroupBox("Polling Rate", this);
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

    pollForm->addRow("Polling Rate:", pollSliderLayout);

    auto pollSubLbl = new QLabel("Adjust the frequency of UI updates for meters and spectrum.", pollGroup);
    pollSubLbl->setWordWrap(true);
    {
        QFont font = pollSubLbl->font();
        font.setPointSize(11);
        pollSubLbl->setFont(font);
        QPalette pal = pollSubLbl->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        pollSubLbl->setPalette(pal);
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

    silenceForm->addRow("Silence Threshold:", threshSliderLayout);

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

    m_silenceTimeoutLabel = new QLabel("Disabled", silenceGroup);
    m_silenceTimeoutLabel->setFont(monoFont);
    m_silenceTimeoutLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_silenceTimeoutLabel->setMinimumWidth(60);
    timeoutSliderLayout->addWidget(m_silenceTimeoutLabel);

    silenceForm->addRow("Silence Timeout:", timeoutSliderLayout);

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
    silenceSubLbl->setWordWrap(true);
    {
        QFont font = silenceSubLbl->font();
        font.setPointSize(11);
        silenceSubLbl->setFont(font);
        QPalette pal = silenceSubLbl->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        silenceSubLbl->setPalette(pal);
    }
    silenceForm->addRow(silenceSubLbl);

    mainLayout->addWidget(silenceGroup);

    // System Tray Group
    auto trayGroup = new QGroupBox("System Tray", this);
    auto trayLayout = new QVBoxLayout(trayGroup);
    trayLayout->setSpacing(8);

    m_closeToTrayCheck = new QCheckBox("Close window to system tray", trayGroup);
    m_closeToTrayCheck->setChecked(m_settings ? m_settings->closeToTray : true);
    connect(m_closeToTrayCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_settings) {
            m_settings->closeToTray = checked;
            m_settings->savePreferences();
        }
    });
    trayLayout->addWidget(m_closeToTrayCheck);

    m_minimizeToTrayCheck = new QCheckBox("Minimize window to system tray", trayGroup);
    m_minimizeToTrayCheck->setChecked(m_settings ? m_settings->minimizeToTray : false);
    connect(m_minimizeToTrayCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_settings) {
            m_settings->minimizeToTray = checked;
            m_settings->savePreferences();
        }
    });
    trayLayout->addWidget(m_minimizeToTrayCheck);

    mainLayout->addWidget(trayGroup);
    mainLayout->addStretch();
}

void GeneralSettingsView::refreshUi() {
    if (m_settings) {
        if (m_themeCombo) {
            m_themeCombo->blockSignals(true);
            int idx = m_themeCombo->findData(static_cast<int>(m_settings->appTheme));
            if (idx >= 0) {
                m_themeCombo->setCurrentIndex(idx);
            }
            m_themeCombo->blockSignals(false);
        }

        if (m_closeToTrayCheck) {
            m_closeToTrayCheck->blockSignals(true);
            m_closeToTrayCheck->setChecked(m_settings->closeToTray);
            m_closeToTrayCheck->blockSignals(false);
        }
        if (m_minimizeToTrayCheck) {
            m_minimizeToTrayCheck->blockSignals(true);
            m_minimizeToTrayCheck->setChecked(m_settings->minimizeToTray);
            m_minimizeToTrayCheck->blockSignals(false);
        }

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
