#include "ui/MeasurementPositionRowWidget.h"
#include <QHBoxLayout>
#include <QStyle>

MeasurementPositionRowWidget::MeasurementPositionRowWidget(
    const MeasurementPosition& position,
    MeasurementSession* session,
    QWidget* parent
) : QWidget(parent), m_id(position.id), m_session(session) {

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(6);

    // Checkbox to toggle inclusion in averaging
    m_enableCheck = new QCheckBox(this);
    m_enableCheck->setChecked(position.isEnabled);
    m_enableCheck->setToolTip("Include/Exclude position in averaging");
    layout->addWidget(m_enableCheck);

    // Editable name field
    m_nameEdit = new QLineEdit(QString::fromStdString(position.name), this);
    m_nameEdit->setPlaceholderText("Position Name");
    m_nameEdit->setMinimumWidth(100);
    layout->addWidget(m_nameEdit);

    // Channel Kind Combo
    m_kindCombo = new QComboBox(this);
    m_kindCombo->addItem("Full Range", static_cast<int>(MeasurementChannelKind::Full));
    m_kindCombo->addItem("Mains Only", static_cast<int>(MeasurementChannelKind::Mains));
    m_kindCombo->addItem("Subwoofer Only", static_cast<int>(MeasurementChannelKind::Subwoofer));
    m_kindCombo->setCurrentIndex(static_cast<int>(position.kind));
    layout->addWidget(m_kindCombo);

    // Delete Button
    m_deleteBtn = new QToolButton(this);
    m_deleteBtn->setText("✕");
    m_deleteBtn->setToolTip("Delete this position");
    m_deleteBtn->setStyleSheet("QToolButton { color: #ff5555; border: none; font-weight: bold; } QToolButton:hover { color: #ff0000; }");
    layout->addWidget(m_deleteBtn);

    // Signal connections
    connect(m_enableCheck, &QCheckBox::toggled, [this](bool) {
        if (m_session) m_session->togglePosition(m_id);
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
        }
    });

    connect(m_kindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (m_session) {
            m_session->setPositionKind(m_id, static_cast<MeasurementChannelKind>(idx));
        }
    });

    connect(m_deleteBtn, &QToolButton::clicked, [this]() {
        if (m_session) {
            m_session->removePosition(m_id);
        }
    });
}
