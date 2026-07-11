#ifndef DEVICE_PICKER_VIEW_H
#define DEVICE_PICKER_VIEW_H

#include <QWidget>
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>

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

    // Capture Controls
    QComboBox* m_capBackendCombo;
    QStackedWidget* m_capStack;
    QComboBox* m_capDeviceCombo;
    QListWidget* m_capDeviceList;
    QSpinBox* m_capDevChannelsSpin;
    QSpinBox* m_capStreamChannelsSpin;
    QComboBox* m_capRateCombo;
    QComboBox* m_capFormatCombo;
    QCheckBox* m_bypassDoPCheck;
    QComboBox* m_dopCutoffCombo;

    // Capture File / Generator
    QLineEdit* m_capFilePathEdit;
    QComboBox* m_capFileFormatCombo;
    QSpinBox* m_capFileChannelsSpin;
    QSpinBox* m_capSkipBytesSpin;
    QSpinBox* m_capReadBytesSpin;
    QSpinBox* m_capExtraSamplesSpin;

    QComboBox* m_genTypeCombo;
    QSpinBox* m_genChannelsSpin;
    QDoubleSpinBox* m_genFreqSpin;
    QDoubleSpinBox* m_genLevelSpin;

    // Playback Controls
    QComboBox* m_pbBackendCombo;
    QStackedWidget* m_pbStack;
    QComboBox* m_pbDeviceCombo;
    QListWidget* m_pbDeviceList;
    QSpinBox* m_pbDevChannelsSpin;
    QSpinBox* m_pbStreamChannelsSpin;
    QComboBox* m_pbRateCombo;
    QComboBox* m_pbFormatCombo;
    QCheckBox* m_exclusiveModeCheck;
    QCheckBox* m_outputDoPCheck;
    QComboBox* m_sdmFilterCombo;

    QLineEdit* m_pbFilePathEdit;
    QComboBox* m_pbFileFormatCombo;
    QSpinBox* m_pbFileChannelsSpin;

    // Processing Settings
    QComboBox* m_chunkSizeCombo;
    QCheckBox* m_enableRateAdjustCheck;
    QSpinBox* m_queueLimitSpin;
    QCheckBox* m_stopOnRateChangeCheck;
    QDoubleSpinBox* m_measureIntervalSpin;
    QCheckBox* m_multithreadedCheck;
    QSpinBox* m_workerThreadsSpin;

    void setupUi();
    QWidget* createCapCoreAudioView();
    QWidget* createCapFileView(bool isWav);
    QWidget* createCapGeneratorView();

    QWidget* createPbCoreAudioView();
    QWidget* createPbFileView();
};

#endif // DEVICE_PICKER_VIEW_H
