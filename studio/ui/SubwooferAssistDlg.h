#ifndef SUBWOOFER_ASSIST_DLG_H
#define SUBWOOFER_ASSIST_DLG_H

#include "models/PipelineStore.h"
#include "room_correction/MeasurementSession.h"
#include "room_correction/SubwooferAssist.h"

#include <QDialog>
#include <memory>
#include <optional>

class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QPushButton;

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

    QComboBox* m_mainsPosCombo = nullptr;
    QComboBox* m_subPosCombo = nullptr;

    QGroupBox* m_resultsGroup = nullptr;
    QLabel* m_crossoverValLabel = nullptr;
    QLabel* m_subDelayValLabel = nullptr;
    QLabel* m_mainsHpValLabel = nullptr;
    QLabel* m_subLpValLabel = nullptr;
    QLabel* m_confidenceValLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QPushButton* m_applyDelayBtn = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;

    void updateUi();
    void populatePositionCombos();
};

#endif // SUBWOOFER_ASSIST_DLG_H
