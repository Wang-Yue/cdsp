#include "ui/MeasurementPositionRowWidget.h"

#include <QHBoxLayout>
#include <QStyle>

MeasurementPositionRowWidget::MeasurementPositionRowWidget(const MeasurementPosition& position,
                                                           MeasurementSession* session, QWidget* parent)
    : QWidget(parent), m_id(position.id), m_session(session) {

    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(6);

    auto updateStyle = [this](bool enabled) {
        QString bg = enabled ? "rgba(255, 255, 255, 0.06)" : "rgba(255, 255, 255, 0.02)";
        QString textCol = enabled ? "#ffffff" : "#888888";
        setStyleSheet(QString("MeasurementPositionRowWidget { "
                              "  background-color: %1; "
                              "  border-radius: 6px; "
                              "  border: 1px solid rgba(255, 255, 255, 0.08); "
                              "} "
                              "QLineEdit { background: transparent; border: none; font-family: monospace; font-size: "
                              "11px; color: %2; } "
                              "QComboBox { font-size: 10px; font-family: monospace; padding: 2px 4px; }")
                          .arg(bg, textCol));
    };

    updateStyle(position.isEnabled);

    m_enableCheck = new QCheckBox(this);
    m_enableCheck->setChecked(position.isEnabled);
    m_enableCheck->setToolTip("Include/Exclude position in averaging");
    layout->addWidget(m_enableCheck);

    m_nameEdit = new QLineEdit(QString::fromStdString(position.name), this);
    m_nameEdit->setPlaceholderText("Position Name");
    m_nameEdit->setMinimumWidth(80);
    m_nameEdit->setMaximumWidth(140);
    layout->addWidget(m_nameEdit);

    m_kindCombo = new QComboBox(this);
    m_kindCombo->addItem("Full Range", static_cast<int>(MeasurementChannelKind::Full));
    m_kindCombo->addItem("Mains Only", static_cast<int>(MeasurementChannelKind::Mains));
    m_kindCombo->addItem("Subwoofer Only", static_cast<int>(MeasurementChannelKind::Subwoofer));
    m_kindCombo->setCurrentIndex(static_cast<int>(position.kind));
    layout->addWidget(m_kindCombo);

    m_deleteBtn = new QToolButton(this);
    m_deleteBtn->setText("✕");
    m_deleteBtn->setToolTip("Delete this position");
    m_deleteBtn->setStyleSheet("QToolButton { color: #888; border: none; font-weight: bold; font-size: 11px; } "
                               "QToolButton:hover { color: #ff5555; }");
    layout->addWidget(m_deleteBtn);

    connect(m_enableCheck, &QCheckBox::toggled, [this, updateStyle](bool checked) {
        updateStyle(checked);
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
