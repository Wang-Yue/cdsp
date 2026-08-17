#ifndef CONVOLUTION_PRESET_DETAIL_VIEW_H
#define CONVOLUTION_PRESET_DETAIL_VIEW_H

#include "models/AudioDeviceManager.h"
#include "models/ConvolutionPreset.h"
#include "models/PipelineStore.h"
#include "ui/ConvolutionIRPlot.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <memory>

class ConvolutionPresetDetailView : public QWidget {
    Q_OBJECT

public:
    ConvolutionPresetDetailView(ConvolutionPreset preset, std::shared_ptr<PipelineStore> pipeline,
                                std::shared_ptr<AudioDeviceManager> devices = nullptr, QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void onDeleteClicked();

private:
    ConvolutionPreset m_preset;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<AudioDeviceManager> m_devices;
    int m_previewRate = 48000;

    QLineEdit* m_nameEdit = nullptr;
    QLabel* m_kindLabel = nullptr;
    QLabel* m_tapsLabel = nullptr;
    QLabel* m_ratesLabel = nullptr;
    QLabel* m_latencyKeyLabel = nullptr;
    QLabel* m_latencyValueLabel = nullptr;

    QWidget* m_rateBoxWidget = nullptr;
    QComboBox* m_ratePreviewCombo = nullptr;
    ConvolutionIRPlot* m_irPlot = nullptr;
    QLabel* m_noIrLabel = nullptr;

    QGroupBox* m_filesGroup = nullptr;
    QFormLayout* m_filesForm = nullptr;

    void setupUi();
};

#endif // CONVOLUTION_PRESET_DETAIL_VIEW_H
