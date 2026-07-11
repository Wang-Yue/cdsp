#ifndef DEVICE_PICKER_VIEW_H
#define DEVICE_PICKER_VIEW_H

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QSlider>
#include <memory>
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"

class DevicePickerView : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY(DevicePickerView)

public:
    DevicePickerView(
        std::shared_ptr<AudioDeviceManager> devices,
        std::shared_ptr<AudioSettings> settings,
        QWidget* parent = nullptr
    );

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    bool m_isRefreshing = false;

    // Capture Controls
    QComboBox* m_capBackendCombo;
    QStackedWidget* m_capStack;

    // Capture CoreAudio
    QWidget* m_capWarningWidget;
    QListWidget* m_capDeviceList;
    QComboBox* m_capDevChannelsCombo;
    QSpinBox* m_capDevChannelsSpin;
    QSpinBox* m_capStreamChannelsSpin;
    QComboBox* m_capRateCombo;
    QLabel* m_capRateLabel;
    QComboBox* m_capFormatCombo;
    QLabel* m_capFormatLabel;
    QCheckBox* m_bypassDoPCheck;
    QLabel* m_dopCutoffLabel;
    QComboBox* m_dopCutoffCombo;
    QLabel* m_dopCutoffHint;

    // Capture File (RawFile & WavFile)
    QLineEdit* m_capRawFilePathEdit;
    QComboBox* m_capRawFileFormatCombo;
    QSpinBox* m_capRawFileChannelsSpin;
    QSpinBox* m_capRawSkipBytesSpin;
    QSpinBox* m_capRawReadBytesSpin;
    QSpinBox* m_capRawExtraSamplesSpin;

    QLineEdit* m_capWavFilePathEdit;
    QSpinBox* m_capWavSkipBytesSpin;
    QSpinBox* m_capWavReadBytesSpin;
    QSpinBox* m_capWavExtraSamplesSpin;

    // Capture Generator
    QComboBox* m_genTypeCombo;
    QSpinBox* m_genChannelsSpin;
    QLabel* m_genFreqLabel;
    QDoubleSpinBox* m_genFreqSpin;
    QSlider* m_genFreqSlider;
    QDoubleSpinBox* m_genLevelSpin;
    QSlider* m_genLevelSlider;

    // Playback Controls
    QComboBox* m_pbBackendCombo;
    QStackedWidget* m_pbStack;

    // Playback CoreAudio
    QWidget* m_pbWarningWidget;
    QListWidget* m_pbDeviceList;
    QComboBox* m_pbDevChannelsCombo;
    QSpinBox* m_pbDevChannelsSpin;
    QSpinBox* m_pbStreamChannelsSpin;
    QComboBox* m_pbRateCombo;
    QComboBox* m_pbFormatCombo;
    QLabel* m_pbFormatLabel;
    QCheckBox* m_exclusiveModeCheck;
    QLabel* m_exclusiveModeHint;
    QCheckBox* m_outputDoPCheck;
    QLabel* m_sdmFilterLabel;
    QComboBox* m_sdmFilterCombo;
    QLabel* m_pbDopHintLabel;

    // Playback File
    QLineEdit* m_pbRawFilePathEdit;
    QComboBox* m_pbRawFileFormatCombo;
    QSpinBox* m_pbRawFileChannelsSpin;

    // Processing Settings
    QComboBox* m_chunkSizeCombo;
    QLabel* m_latencyLabel;
    QCheckBox* m_enableRateAdjustCheck;
    QLabel* m_rateAdjustSub;
    QSpinBox* m_queueLimitSpin;
    QCheckBox* m_stopOnRateChangeCheck;
    QDoubleSpinBox* m_measureIntervalSpin;
    QSlider* m_measureIntervalSlider;
    QLabel* m_measureIntervalValLabel;
    QCheckBox* m_multithreadedCheck;
    QWidget* m_workerThreadsRow;
    QSpinBox* m_workerThreadsSpin;

    void setupUi();
    QWidget* createCapCoreAudioView();
    QWidget* createCapFileView(bool isWav);
    QWidget* createCapGeneratorView();

    QWidget* createPbCoreAudioView();
    QWidget* createPbFileView();

    static QString formatSampleRate(int rate);
    void updateDoPCapability();
    void updateLatencyText();
    void populateDeviceList(
        QListWidget* listWidget,
        QWidget* warningWidget,
        const std::vector<AudioDevice>& devices,
        const std::optional<std::string>& selectedDeviceName
    );
};

#endif // DEVICE_PICKER_VIEW_H
