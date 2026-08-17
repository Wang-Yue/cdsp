#ifndef MEASUREMENT_POSITION_ROW_WIDGET_H
#define MEASUREMENT_POSITION_ROW_WIDGET_H

#include "room_correction/MeasurementSession.h"

#include <QUuid>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QToolButton;

class MeasurementPositionRowWidget : public QWidget {
    Q_OBJECT

public:
    explicit MeasurementPositionRowWidget(const MeasurementPosition& position, MeasurementSession* session,
                                          QWidget* parent = nullptr);

signals:
    void positionChanged();

private:
    QUuid m_id;
    MeasurementSession* m_session = nullptr;

    QCheckBox* m_enableCheck = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_kindCombo = nullptr;
    QToolButton* m_deleteBtn = nullptr;
};

#endif // MEASUREMENT_POSITION_ROW_WIDGET_H
