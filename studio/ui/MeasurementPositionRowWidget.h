#ifndef MEASUREMENT_POSITION_ROW_WIDGET_H
#define MEASUREMENT_POSITION_ROW_WIDGET_H

#include "room_correction/MeasurementSession.h"
#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QToolButton>
#include <QUuid>

class MeasurementPositionRowWidget : public QWidget {
    Q_OBJECT

public:
    explicit MeasurementPositionRowWidget(
        const MeasurementPosition& position,
        MeasurementSession* session,
        QWidget* parent = nullptr
    );

signals:
    void positionChanged();

private:
    QUuid m_id;
    MeasurementSession* m_session;

    QCheckBox* m_enableCheck;
    QLineEdit* m_nameEdit;
    QComboBox* m_kindCombo;
    QToolButton* m_deleteBtn;
};

#endif // MEASUREMENT_POSITION_ROW_WIDGET_H
