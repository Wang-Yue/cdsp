#ifndef STAGE_DETAIL_VIEW_H
#define STAGE_DETAIL_VIEW_H

#include "models/DSPEngineController.h"
#include "models/PipelineStage.h"
#include "models/PipelineStore.h"
#include "ui/ConvolutionIRPlot.h"
#include "ui/EQDiagramWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QWidget>
#include <functional>
#include <memory>

class VSliderWidget : public QWidget {
    Q_OBJECT
public:
    explicit VSliderWidget(double value = 0.0, double minVal = -40.0, double maxVal = 40.0, QWidget* parent = nullptr);

    double value() const { return m_value; }
    void setValue(double val);

signals:
    void valueChanged(double newVal);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    double m_value;
    double m_minVal;
    double m_maxVal;

    void updateValueFromMouse(int y);
};

class RotatedLabel : public QWidget {
    Q_OBJECT
public:
    explicit RotatedLabel(const QString& text = "", QWidget* parent = nullptr);
    void setText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    QString m_text;
};

class StageDetailView : public QWidget {
    Q_OBJECT

public:
    StageDetailView(size_t stageIndex, std::shared_ptr<PipelineStore> pipeline,
                    std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void applyConfig();

private:
    size_t m_stageIndex;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;

    QLineEdit* m_nameEdit = nullptr;
    QCheckBox* m_enabledCheck = nullptr;
    QWidget* m_optionsContainer = nullptr;
    bool m_isBuildingUi = false;
    bool m_isLocalEditing = false;

    void setupUi();
    void buildStageOptionsUi();
    PipelineStage* currentStage() const;

    QWidget* createMatrixCellWidget(PipelineStage& stage, int dest, int src, QTableWidget* table);
    QWidget* createChannelSelectorWidget(int incomingChannels, const std::function<std::vector<int>()>& getter,
                                         const std::function<void(const std::vector<int>&)>& setter,
                                         QWidget* parent = nullptr);
};

#endif // STAGE_DETAIL_VIEW_H
