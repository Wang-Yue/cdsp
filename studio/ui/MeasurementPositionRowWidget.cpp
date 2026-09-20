#include "ui/MeasurementPositionRowWidget.h"

#include <QCheckBox>   // for QCheckBox
#include <QComboBox>   // for QComboBox
#include <QHBoxLayout> // for QHBoxLayout
#include <QLineEdit>   // for QLineEdit
#include <QString>     // for QString
#include <QToolButton> // for QToolButton
#include <QtGlobal>    // for QOverload
#include <string>      // for basic_string
#include <vector>      // for vector

MeasurementPositionRowWidget::MeasurementPositionRowWidget(const MeasurementPosition& position,
                                                           MeasurementSession* session, QWidget* parent)
    : QWidget(parent), m_id(position.id), m_session(session) {

    auto layout = new QHBoxLayout(this);

    m_enableCheck = new QCheckBox(this);
    m_enableCheck->setChecked(position.isEnabled);
    m_enableCheck->setToolTip("Include/Exclude position in averaging");
    layout->addWidget(m_enableCheck);

    m_nameEdit = new QLineEdit(QString::fromStdString(position.name), this);
    m_nameEdit->setPlaceholderText("Position Name");
    layout->addWidget(m_nameEdit, 1);

    m_kindCombo = new QComboBox(this);
    m_kindCombo->addItem("Full Range", static_cast<int>(MeasurementChannelKind::Full));
    m_kindCombo->addItem("Mains Only", static_cast<int>(MeasurementChannelKind::Mains));
    m_kindCombo->addItem("Subwoofer Only", static_cast<int>(MeasurementChannelKind::Subwoofer));
    m_kindCombo->setCurrentIndex(static_cast<int>(position.kind));
    layout->addWidget(m_kindCombo);

    m_deleteBtn = new QToolButton(this);
    m_deleteBtn->setText("✕");
    m_deleteBtn->setToolTip("Delete this position");
    layout->addWidget(m_deleteBtn);

    connect(m_enableCheck, &QCheckBox::toggled, [this](bool) {
        if (m_session) {
            m_session->togglePosition(m_id);
            emit positionChanged();
        }
    });

    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        if (m_session) {
            for (auto& p : m_session->positions) {
                if (p.id == m_id) {
                    p.name = m_nameEdit->text().toStdString();
                    break;
                }
            }
            emit m_session->sessionUpdated();
            emit positionChanged();
        }
    });

    connect(m_kindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (m_session) {
            m_session->setPositionKind(m_id, static_cast<MeasurementChannelKind>(idx));
            emit positionChanged();
        }
    });

    connect(m_deleteBtn, &QToolButton::clicked, [this]() {
        if (m_session) {
            m_session->removePosition(m_id);
            emit positionChanged();
        }
    });
}
