#ifndef DEVICE_PICKER_VIEW_H
#define DEVICE_PICKER_VIEW_H

#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

class DevicePickerView : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY(DevicePickerView)

public:
    DevicePickerView(std::shared_ptr<AudioDeviceManager> devices, std::shared_ptr<AudioSettings> settings,
                     QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    bool m_isRefreshing = false;

    // Capture Controls
    QComboBox* m_capBackendCombo = nullptr;
    QStackedWidget* m_capStack = nullptr;

    // Capture CoreAudio
    QWidget* m_capWarningWidget = nullptr;
    QWidget* m_capDeviceListContainer = nullptr;
    QVBoxLayout* m_capDeviceListLayout = nullptr;
    QComboBox* m_capDevChannelsCombo = nullptr;
    QSpinBox* m_capDevChannelsSpin = nullptr;
    QSpinBox* m_capStreamChannelsSpin = nullptr;
    QComboBox* m_capRateCombo = nullptr;
    QLabel* m_capRateLabel = nullptr;
    QComboBox* m_capFormatCombo = nullptr;
    QLabel* m_capFormatLabel = nullptr;
    QCheckBox* m_bypassDoPCheck = nullptr;
    QFrame* m_capDopDivider = nullptr;
    QLabel* m_dopCutoffLabel = nullptr;
    QComboBox* m_dopCutoffCombo = nullptr;
    QWidget* m_capDopCutoffRow = nullptr;
    QLabel* m_dopCutoffHint = nullptr;

    // Capture File (RawFile & WavFile)
    QLineEdit* m_capRawFilePathEdit = nullptr;
    QComboBox* m_capRawFileFormatCombo = nullptr;
    QSpinBox* m_capRawFileChannelsSpin = nullptr;
    QSpinBox* m_capRawSkipBytesSpin = nullptr;
    QSpinBox* m_capRawReadBytesSpin = nullptr;
    QSpinBox* m_capRawExtraSamplesSpin = nullptr;

    QLineEdit* m_capWavFilePathEdit = nullptr;
    QSpinBox* m_capWavSkipBytesSpin = nullptr;
    QSpinBox* m_capWavReadBytesSpin = nullptr;
    QSpinBox* m_capWavExtraSamplesSpin = nullptr;

    // Capture Generator
    QComboBox* m_genTypeCombo = nullptr;
    QSpinBox* m_genChannelsSpin = nullptr;
    QLabel* m_genFreqLabel = nullptr;
    QDoubleSpinBox* m_genFreqSpin = nullptr;
    QSlider* m_genFreqSlider = nullptr;
    QDoubleSpinBox* m_genLevelSpin = nullptr;
    QSlider* m_genLevelSlider = nullptr;

    // Playback Controls
    QComboBox* m_pbBackendCombo = nullptr;
    QStackedWidget* m_pbStack = nullptr;

    // Capture WASAPI / ASIO / ALSA / PipeWire
    QCheckBox* m_capWasapiExclusiveCheck = nullptr;
    QCheckBox* m_capWasapiLoopbackCheck = nullptr;
    QCheckBox* m_capWasapiPollingCheck = nullptr;
    QCheckBox* m_capAlsaStopInactiveCheck = nullptr;
    QWidget* m_capAlsaLinkVolRow = nullptr;
    QLineEdit* m_capAlsaLinkVolumeEdit = nullptr;
    QWidget* m_capAlsaLinkMuteRow = nullptr;
    QLineEdit* m_capAlsaLinkMuteEdit = nullptr;
    QWidget* m_capPipeWireRow = nullptr;
    QLineEdit* m_capPwNodeNameEdit = nullptr;
    QLineEdit* m_capPwNodeDescEdit = nullptr;
    QLineEdit* m_capPwNodeGroupEdit = nullptr;
    QLineEdit* m_capPwAutoconnectEdit = nullptr;

    // Playback CoreAudio / WASAPI / ASIO / ALSA / PipeWire
    QWidget* m_pbWarningWidget = nullptr;
    QWidget* m_pbDeviceListContainer = nullptr;
    QVBoxLayout* m_pbDeviceListLayout = nullptr;
    QComboBox* m_pbDevChannelsCombo = nullptr;
    QSpinBox* m_pbDevChannelsSpin = nullptr;
    QSpinBox* m_pbStreamChannelsSpin = nullptr;
    QComboBox* m_pbRateCombo = nullptr;
    QComboBox* m_pbFormatCombo = nullptr;
    QLabel* m_pbFormatLabel = nullptr;
    QCheckBox* m_exclusiveModeCheck = nullptr;
    QLabel* m_exclusiveModeHint = nullptr;
    QCheckBox* m_pbWasapiPollingCheck = nullptr;
    QCheckBox* m_pbAlsaStopInactiveCheck = nullptr;
    QWidget* m_pbAlsaLinkVolRow = nullptr;
    QLineEdit* m_pbAlsaLinkVolumeEdit = nullptr;
    QWidget* m_pbAlsaLinkMuteRow = nullptr;
    QLineEdit* m_pbAlsaLinkMuteEdit = nullptr;
    QWidget* m_pbPipeWireRow = nullptr;
    QLineEdit* m_pbPwNodeNameEdit = nullptr;
    QLineEdit* m_pbPwNodeDescEdit = nullptr;
    QLineEdit* m_pbPwNodeGroupEdit = nullptr;
    QLineEdit* m_pbPwAutoconnectEdit = nullptr;
    QCheckBox* m_outputDoPCheck = nullptr;
    QFrame* m_pbDopDivider = nullptr;
    QCheckBox* m_outputDSDCheck = nullptr;
    QLabel* m_sdmFilterLabel = nullptr;
    QComboBox* m_sdmFilterCombo = nullptr;
    QWidget* m_pbSdmFilterRow = nullptr;
    QLabel* m_pbDopHintLabel = nullptr;

    // Playback File
    QLineEdit* m_pbRawFilePathEdit = nullptr;
    QComboBox* m_pbRawFileFormatCombo = nullptr;
    QSpinBox* m_pbRawFileChannelsSpin = nullptr;

    QLineEdit* m_pbWavFilePathEdit = nullptr;
    QComboBox* m_pbWavFileFormatCombo = nullptr;
    QSpinBox* m_pbWavFileChannelsSpin = nullptr;
    QComboBox* m_pbWavUseRf64Combo = nullptr;

    // Processing Settings
    QComboBox* m_chunkSizeCombo = nullptr;
    QLabel* m_latencyLabel = nullptr;
    QCheckBox* m_enableRateAdjustCheck = nullptr;
    QLabel* m_rateAdjustSub = nullptr;
    QSpinBox* m_queueLimitSpin = nullptr;
    QCheckBox* m_stopOnRateChangeCheck = nullptr;
    QSlider* m_measureIntervalSlider = nullptr;
    QLabel* m_measureIntervalValLabel = nullptr;
    QCheckBox* m_multithreadedCheck = nullptr;
    QWidget* m_workerThreadsRow = nullptr;
    QSpinBox* m_workerThreadsSpin = nullptr;

    void setupUi();
    QWidget* createCapCoreAudioView();
    QWidget* createCapFileView(bool isWav);
    QWidget* createCapGeneratorView();

    QWidget* createPbCoreAudioView();
    QWidget* createPbFileView(bool isWav);

    static QString formatSampleRate(int rate);
    void updateDoPCapability();
    void updateLatencyText();
    void populateDeviceList(QVBoxLayout* listLayout, QWidget* warningWidget, QWidget* containerWidget,
                            const std::vector<AudioDevice>& devices,
                            const std::optional<std::string>& selectedDeviceName, bool isCapture);
};

#endif // DEVICE_PICKER_VIEW_H
