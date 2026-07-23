#ifndef SUBWOOFER_ASSIST_DLG_H
#define SUBWOOFER_ASSIST_DLG_H

#include "models/PipelineStore.h"
#include "room_correction/MeasurementSession.h"
#include "room_correction/SubwooferAssist.h"

#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <memory>
#include <optional>

class SubwooferAssistDlg : public QDialog {
    Q_OBJECT

public:
    explicit SubwooferAssistDlg(MeasurementSession* session, std::shared_ptr<PipelineStore> pipeline = nullptr,
                                QWidget* parent = nullptr);

private slots:
    void onRecommendClicked();
    void onApplyDelayToPipeline();

private:
    MeasurementSession* m_session;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::optional<SubwooferRecommendation> m_recommendation;

    QComboBox* m_mainsPosCombo;
    QComboBox* m_subPosCombo;

    QLabel* m_crossoverValLabel;
    QLabel* m_subDelayValLabel;
    QLabel* m_mainsHpValLabel;
    QLabel* m_subLpValLabel;
    QLabel* m_confidenceValLabel;
    QLabel* m_summaryLabel;
    QPushButton* m_applyDelayBtn;
    QWidget* m_resultsWidget;

    void updateUi();
    void populatePositionCombos();
    QWidget* createMetaCell(const QString& title, QLabel** valueLabelOut);
};

#endif // SUBWOOFER_ASSIST_DLG_H
