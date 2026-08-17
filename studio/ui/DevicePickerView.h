#ifndef DEVICE_PICKER_VIEW_H
#define DEVICE_PICKER_VIEW_H

#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
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

/* clang-format off */
/**
 * @brief DevicePickerView - Audio device selection and backend configuration UI.
 *
 * Backend Option Support Matrix:
 * ┌─────────────┬──────────┬──────────────┬────────────┬────────────┬────────┬──────────────┬───────────┬─────────┬───────────┬──────────┬───────────┐
 * │ Backend     │ Mode     │ Device List  │ Dev Chans  │ Stream Chs │ Rate   │ Format       │ Exclusive │ Polling │ ALSA Link │ PipeWire │ DoP & SDM │
 * │             │          │ & Warnings   │ (Hardware) │            │        │ (Bit-depth)  │ (Hog)     │ (WASAPI)│ Vol / Mute│ Nodes    │ Filters   │
 * ├─────────────┼──────────┼──────────────┼────────────┼────────────┼────────┼──────────────┼───────────┼─────────┼───────────┼──────────┼───────────┤
 * │ CoreAudio   │ Capture  │     YES      │    YES     │    YES     │  YES   │     YES      │    NO     │   NO    │    NO     │    NO    │  YES (C)  │
 * │ CoreAudio   │ Playback │     YES      │    YES     │    YES     │  YES   │     YES      │    YES    │   NO    │    NO     │    NO    │  YES (C)  │
 * │ WASAPI      │ Capture  │     YES      │    YES     │    YES     │  YES   │     YES      │    YES    │   YES   │    NO     │    NO    │  YES (C)  │
 * │ WASAPI      │ Playback │     YES      │    YES     │    YES     │  YES   │     YES      │    YES    │   YES   │    NO     │    NO    │  YES (C)  │
 * │ ASIO        │ Cap / Pb │     YES      │    YES     │    YES     │  YES   │     YES      │    NO     │   NO    │    NO     │    NO    │  YES (C)  │
 * │ ALSA        │ Cap / Pb │     YES      │    YES     │    YES     │  YES   │     YES      │    NO     │   NO    │    YES    │    NO    │  YES (C)  │
 * │ PipeWire    │ Cap / Pb │      NO      │     NO     │    YES     │  YES   │      NO      │    NO     │   NO    │    NO     │   YES    │    NO     │
 * │ RawFile     │ Cap / Pb │      NO      │     NO     │    YES     │   NO   │     YES      │    NO     │   NO    │    NO     │    NO    │    NO     │
 * │ WavFile     │ Cap / Pb │      NO      │     NO     │NO(Cap)/Y(Pb│   NO   │ NO(Cap)/Y(Pb)│    NO     │   NO    │    NO     │    NO    │    NO     │
 * │ SignalGen   │ Capture  │      NO      │     NO     │    YES     │   NO   │      NO      │    NO     │   NO    │    NO     │    NO    │    NO     │
 * └─────────────┴──────────┴──────────────┴────────────┴────────────┴────────┴──────────────┴───────────┴─────────┴───────────┴──────────┴───────────┘
 * Note: Native DSD is selected directly via the `format` option on supported backends (e.g. ALSA / ASIO).
 */
/* clang-format on */
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
    QFormLayout* m_capBackendForm = nullptr;
    QComboBox* m_capBackendCombo = nullptr;
    QStackedWidget* m_capStack = nullptr;

    // Capture CoreAudio / WASAPI / ASIO / ALSA / PipeWire
    QFormLayout* m_capCoreAudioForm = nullptr;
    QWidget* m_capDeviceContainer = nullptr;
    QLabel* m_capWarningLabel = nullptr;
    QListWidget* m_capDeviceList = nullptr;
    QWidget* m_capDevChannelsRow = nullptr;
    QComboBox* m_capDevChannelsCombo = nullptr;
    QSpinBox* m_capDevChannelsSpin = nullptr;
    QSpinBox* m_capStreamChannelsSpin = nullptr;
    QWidget* m_capRateRow = nullptr;
    QComboBox* m_capRateCombo = nullptr;
    QLabel* m_capRateLabel = nullptr;
    QWidget* m_capFormatRow = nullptr;
    QComboBox* m_capFormatCombo = nullptr;
    QLabel* m_capFormatLabel = nullptr;
    QCheckBox* m_bypassDoPCheck = nullptr;
    QComboBox* m_dopCutoffCombo = nullptr;
    QLabel* m_dopCutoffHint = nullptr;

    // Capture WASAPI / ASIO / ALSA / PipeWire
    QCheckBox* m_capWasapiExclusiveCheck = nullptr;
    QCheckBox* m_capWasapiLoopbackCheck = nullptr;
    QCheckBox* m_capWasapiPollingCheck = nullptr;
    QCheckBox* m_capAlsaStopInactiveCheck = nullptr;
    QCheckBox* m_capAlsaThreadedCheck = nullptr;
    QLineEdit* m_capAlsaLinkVolumeEdit = nullptr;
    QLineEdit* m_capAlsaLinkMuteEdit = nullptr;
    QLineEdit* m_capPwNodeNameEdit = nullptr;
    QLineEdit* m_capPwNodeDescEdit = nullptr;
    QLineEdit* m_capPwNodeGroupEdit = nullptr;
    QLineEdit* m_capPwAutoconnectEdit = nullptr;

    // Capture File (RawFile & WavFile)
    QFormLayout* m_capRawFileForm = nullptr;
    QLineEdit* m_capRawFilePathEdit = nullptr;
    QComboBox* m_capRawFileFormatCombo = nullptr;
    QSpinBox* m_capRawFileChannelsSpin = nullptr;
    QSpinBox* m_capRawSkipBytesSpin = nullptr;
    QSpinBox* m_capRawReadBytesSpin = nullptr;
    QSpinBox* m_capRawExtraSamplesSpin = nullptr;

    QFormLayout* m_capWavFileForm = nullptr;
    QLineEdit* m_capWavFilePathEdit = nullptr;
    QSpinBox* m_capWavSkipBytesSpin = nullptr;
    QSpinBox* m_capWavReadBytesSpin = nullptr;
    QSpinBox* m_capWavExtraSamplesSpin = nullptr;

    // Capture Generator
    QFormLayout* m_capGenForm = nullptr;
    QComboBox* m_genTypeCombo = nullptr;
    QSpinBox* m_genChannelsSpin = nullptr;
    QDoubleSpinBox* m_genFreqSpin = nullptr;
    QSlider* m_genFreqSlider = nullptr;
    QDoubleSpinBox* m_genLevelSpin = nullptr;
    QSlider* m_genLevelSlider = nullptr;

    // Playback Controls
    QFormLayout* m_pbBackendForm = nullptr;
    QComboBox* m_pbBackendCombo = nullptr;
    QStackedWidget* m_pbStack = nullptr;

    // Playback CoreAudio / WASAPI / ASIO / ALSA / PipeWire
    QFormLayout* m_pbCoreAudioForm = nullptr;
    QWidget* m_pbDeviceContainer = nullptr;
    QLabel* m_pbWarningLabel = nullptr;
    QListWidget* m_pbDeviceList = nullptr;
    QWidget* m_pbDevChannelsRow = nullptr;
    QComboBox* m_pbDevChannelsCombo = nullptr;
    QSpinBox* m_pbDevChannelsSpin = nullptr;
    QSpinBox* m_pbStreamChannelsSpin = nullptr;
    QComboBox* m_pbRateCombo = nullptr;
    QWidget* m_pbFormatRow = nullptr;
    QComboBox* m_pbFormatCombo = nullptr;
    QLabel* m_pbFormatLabel = nullptr;
    QCheckBox* m_exclusiveModeCheck = nullptr;
    QLabel* m_exclusiveModeHint = nullptr;
    QCheckBox* m_pbWasapiPollingCheck = nullptr;
    QCheckBox* m_pbAlsaThreadedCheck = nullptr;
    QLineEdit* m_pbPwNodeNameEdit = nullptr;
    QLineEdit* m_pbPwNodeDescEdit = nullptr;
    QLineEdit* m_pbPwNodeGroupEdit = nullptr;
    QLineEdit* m_pbPwAutoconnectEdit = nullptr;
    QCheckBox* m_outputDoPCheck = nullptr;
    QComboBox* m_sdmFilterCombo = nullptr;
    QLabel* m_pbDopHintLabel = nullptr;

    // Playback File
    QFormLayout* m_pbRawFileForm = nullptr;
    QLineEdit* m_pbRawFilePathEdit = nullptr;
    QComboBox* m_pbRawFileFormatCombo = nullptr;
    QSpinBox* m_pbRawFileChannelsSpin = nullptr;

    QFormLayout* m_pbWavFileForm = nullptr;
    QLineEdit* m_pbWavFilePathEdit = nullptr;
    QComboBox* m_pbWavFileFormatCombo = nullptr;
    QSpinBox* m_pbWavFileChannelsSpin = nullptr;
    QComboBox* m_pbWavUseRf64Combo = nullptr;

    // Processing Settings
    QFormLayout* m_procForm = nullptr;
    QComboBox* m_chunkSizeCombo = nullptr;
    QLabel* m_latencyLabel = nullptr;
    QCheckBox* m_enableRateAdjustCheck = nullptr;
    QLabel* m_rateAdjustSub = nullptr;
    QSpinBox* m_queueLimitSpin = nullptr;
    QCheckBox* m_stopOnRateChangeCheck = nullptr;
    QSlider* m_measureIntervalSlider = nullptr;
    QLabel* m_measureIntervalValLabel = nullptr;
    QCheckBox* m_multithreadedCheck = nullptr;
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
    void populateDeviceList(QListWidget* listWidget, QWidget* warningWidget, const std::vector<AudioDevice>& devices,
                            const std::optional<std::string>& selectedDeviceName, bool isCapture);
};

#endif // DEVICE_PICKER_VIEW_H
