#ifndef SUBWOOFER_ASSIST_DLG_H
#define SUBWOOFER_ASSIST_DLG_H

#include "room_correction/MeasurementSession.h"
#include "room_correction/SubwooferAssist.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <optional>

class SubwooferAssistDlg : public QDialog {
    Q_OBJECT

public:
    explicit SubwooferAssistDlg(MeasurementSession* session, QWidget* parent = nullptr);

private slots:
    void onRecommendClicked();

private:
    MeasurementSession* m_session;
    std::optional<SubwooferRecommendation> m_recommendation;

    QLabel* m_crossoverValLabel;
    QLabel* m_subDelayValLabel;
    QLabel* m_mainsHpValLabel;
    QLabel* m_subLpValLabel;
    QLabel* m_confidenceValLabel;
    QLabel* m_summaryLabel;
    QWidget* m_resultsWidget;

    void updateUi();
    QWidget* createMetaCell(const QString& title, QLabel** valueLabelOut);
};

#endif // SUBWOOFER_ASSIST_DLG_H
