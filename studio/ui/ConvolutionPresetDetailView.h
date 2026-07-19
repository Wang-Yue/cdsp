#ifndef CONVOLUTION_PRESET_DETAIL_VIEW_H
#define CONVOLUTION_PRESET_DETAIL_VIEW_H

#include "models/AudioDeviceManager.h"
#include "models/ConvolutionPreset.h"
#include "models/PipelineStore.h"
#include "ui/ConvolutionIRPlot.h"

#include <QComboBox>
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

    QLineEdit* m_nameEdit;
    QLabel* m_kindLabel;
    QLabel* m_tapsLabel;
    QLabel* m_ratesLabel;
    QLabel* m_latencyKeyLabel;
    QLabel* m_latencyValueLabel;

    QWidget* m_rateBoxWidget;
    QComboBox* m_ratePreviewCombo;
    ConvolutionIRPlot* m_irPlot;
    QLabel* m_noIrLabel;
    QWidget* m_filesContainer;

    void setupUi();
};

#endif // CONVOLUTION_PRESET_DETAIL_VIEW_H
