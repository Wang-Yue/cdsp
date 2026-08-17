#include "ui/SubwooferAssistDlg.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

SubwooferAssistDlg::SubwooferAssistDlg(MeasurementSession* session, std::shared_ptr<PipelineStore> pipeline,
                                       QWidget* parent)
    : QDialog(parent), m_session(session), m_pipeline(pipeline) {
    setWindowTitle("Subwoofer Crossover Assist");
    resize(480, 420);

    auto mainLayout = new QVBoxLayout(this);

    // Header bar
    auto headerLayout = new QHBoxLayout();
    auto titleLabel = new QLabel("Subwoofer Crossover Assist", this);
    titleLabel->setFont(QFont("", 12, QFont::Bold));
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch(1);

    auto recommendBtn = new QPushButton("Recommend", this);
    connect(recommendBtn, &QPushButton::clicked, this, &SubwooferAssistDlg::onRecommendClicked);
    headerLayout->addWidget(recommendBtn);

    mainLayout->addLayout(headerLayout);

    // Position Selection Section
    auto posGroup = new QGroupBox("Position Selection", this);
    auto posForm = new QFormLayout(posGroup);

    m_mainsPosCombo = new QComboBox(posGroup);
    posForm->addRow("Mains Position:", m_mainsPosCombo);

    m_subPosCombo = new QComboBox(posGroup);
    posForm->addRow("Subwoofer Position:", m_subPosCombo);

    mainLayout->addWidget(posGroup);

    populatePositionCombos();

    connect(m_mainsPosCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int) { onRecommendClicked(); });
    connect(m_subPosCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { onRecommendClicked(); });

    // Results container
    m_resultsGroup = new QGroupBox("Recommendation", this);
    auto resultsLayout = new QGridLayout(m_resultsGroup);

    resultsLayout->addWidget(new QLabel("Crossover:", m_resultsGroup), 0, 0);
    m_crossoverValLabel = new QLabel("—", m_resultsGroup);
    m_crossoverValLabel->setFont(QFont("monospace", 11, QFont::Bold));
    resultsLayout->addWidget(m_crossoverValLabel, 0, 1);

    resultsLayout->addWidget(new QLabel("Confidence:", m_resultsGroup), 0, 2);
    m_confidenceValLabel = new QLabel("—", m_resultsGroup);
    m_confidenceValLabel->setFont(QFont("monospace", 11, QFont::Bold));
    resultsLayout->addWidget(m_confidenceValLabel, 0, 3);

    resultsLayout->addWidget(new QLabel("Mains High-Pass:", m_resultsGroup), 1, 0);
    m_mainsHpValLabel = new QLabel("—", m_resultsGroup);
    m_mainsHpValLabel->setFont(QFont("monospace", 11, QFont::Bold));
    resultsLayout->addWidget(m_mainsHpValLabel, 1, 1);

    resultsLayout->addWidget(new QLabel("Sub Low-Pass:", m_resultsGroup), 1, 2);
    m_subLpValLabel = new QLabel("—", m_resultsGroup);
    m_subLpValLabel->setFont(QFont("monospace", 11, QFont::Bold));
    resultsLayout->addWidget(m_subLpValLabel, 1, 3);

    resultsLayout->addWidget(new QLabel("Sub Delay:", m_resultsGroup), 2, 0);
    m_subDelayValLabel = new QLabel("—", m_resultsGroup);
    m_subDelayValLabel->setFont(QFont("monospace", 11, QFont::Bold));
    resultsLayout->addWidget(m_subDelayValLabel, 2, 1);

    m_summaryLabel = new QLabel(m_resultsGroup);
    m_summaryLabel->setWordWrap(true);
    resultsLayout->addWidget(m_summaryLabel, 3, 0, 1, 4);

    m_applyDelayBtn = new QPushButton("Apply Recommended Delay to Pipeline", m_resultsGroup);
    m_applyDelayBtn->setEnabled(false);
    connect(m_applyDelayBtn, &QPushButton::clicked, this, &SubwooferAssistDlg::onApplyDelayToPipeline);
    resultsLayout->addWidget(m_applyDelayBtn, 4, 0, 1, 4);

    mainLayout->addWidget(m_resultsGroup);

    mainLayout->addStretch(1);

    // Standard dialog button box
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);

    // Run initial recommendation if available
    onRecommendClicked();
}

void SubwooferAssistDlg::populatePositionCombos() {
    m_mainsPosCombo->clear();
    m_subPosCombo->clear();

    if (!m_session)
        return;

    int defaultMainsIdx = -1;
    int defaultSubIdx = -1;

    for (size_t i = 0; i < m_session->positions.size(); ++i) {
        const auto& p = m_session->positions[i];
        if (!p.ir.has_value())
            continue;

        QString itemText = QString::fromStdString(p.name);
        m_mainsPosCombo->addItem(itemText, QVariant::fromValue(static_cast<int>(i)));
        m_subPosCombo->addItem(itemText, QVariant::fromValue(static_cast<int>(i)));

        if (p.kind == MeasurementChannelKind::Mains) {
            defaultMainsIdx = m_mainsPosCombo->count() - 1;
        }
        if (p.kind == MeasurementChannelKind::Subwoofer) {
            defaultSubIdx = m_subPosCombo->count() - 1;
        }
    }

    if (defaultMainsIdx >= 0) {
        m_mainsPosCombo->setCurrentIndex(defaultMainsIdx);
    } else if (m_mainsPosCombo->count() > 0) {
        m_mainsPosCombo->setCurrentIndex(0);
    }

    if (defaultSubIdx >= 0) {
        m_subPosCombo->setCurrentIndex(defaultSubIdx);
    } else if (m_subPosCombo->count() > 1) {
        m_subPosCombo->setCurrentIndex(1);
    } else if (m_subPosCombo->count() > 0) {
        m_subPosCombo->setCurrentIndex(0);
    }
}

void SubwooferAssistDlg::onRecommendClicked() {
    if (!m_session || m_mainsPosCombo->count() == 0 || m_subPosCombo->count() == 0) {
        m_recommendation = std::nullopt;
        updateUi();
        return;
    }

    int mainsPosIdx = m_mainsPosCombo->currentData().toInt();
    int subPosIdx = m_subPosCombo->currentData().toInt();

    if (mainsPosIdx < 0 || mainsPosIdx >= static_cast<int>(m_session->positions.size()) || subPosIdx < 0 ||
        subPosIdx >= static_cast<int>(m_session->positions.size())) {
        m_recommendation = std::nullopt;
        updateUi();
        return;
    }

    const auto& mainsP = m_session->positions[mainsPosIdx];
    const auto& subP = m_session->positions[subPosIdx];

    if (mainsP.ir.has_value() && subP.ir.has_value()) {
        m_recommendation = SubwooferAssist::recommend(mainsP.ir.value(), subP.ir.value());
    } else {
        m_recommendation = std::nullopt;
    }

    updateUi();
}

void SubwooferAssistDlg::onApplyDelayToPipeline() {
    if (!m_recommendation.has_value() || !m_pipeline)
        return;

    double delayMs = std::abs(m_recommendation->subDelayMs);

    // Look for an existing Delay stage
    PipelineStage* delayStage = nullptr;
    for (auto& stage : m_pipeline->stages) {
        if (stage.type == StageType::Delay) {
            delayStage = &stage;
            break;
        }
    }

    if (!delayStage) {
        QUuid newId = m_pipeline->addStage(StageType::Delay);
        for (auto& stage : m_pipeline->stages) {
            if (stage.id == newId) {
                delayStage = &stage;
                delayStage->name = "Subwoofer Delay";
                break;
            }
        }
    }

    if (delayStage) {
        delayStage->delayValue = delayMs;
        delayStage->delayUnit = DelayUnit::ms;
        delayStage->isEnabled = true;
        m_pipeline->savePipelineStages();
        emit m_pipeline->pipelineChanged();

        QMessageBox::information(
            this, "Delay Applied",
            QString("Applied recommended subwoofer delay of %1 ms to pipeline.").arg(delayMs, 0, 'f', 2));
    }
}

void SubwooferAssistDlg::updateUi() {
    if (m_recommendation.has_value()) {
        const auto& r = m_recommendation.value();
        m_crossoverValLabel->setText(QString("%1 Hz").arg(r.crossoverHz, 0, 'f', 0));
        m_subDelayValLabel->setText(QString("%1%2 ms").arg(r.subDelayMs >= 0 ? "+" : "").arg(r.subDelayMs, 0, 'f', 2));
        double mainsHpFreq = r.mainsHighPass.freq.value_or(0.0);
        double subLpFreq = r.subLowPass.freq.value_or(0.0);
        m_mainsHpValLabel->setText(QString("%1 Hz · LR2").arg(static_cast<int>(mainsHpFreq)));
        m_subLpValLabel->setText(QString("%1 Hz · LR2").arg(static_cast<int>(subLpFreq)));
        m_confidenceValLabel->setText(QString("%1%").arg(static_cast<int>(r.confidence * 100.0)));
        m_summaryLabel->setText(QString::fromStdString(r.summary));
        m_applyDelayBtn->setEnabled(m_pipeline != nullptr);
    } else {
        m_crossoverValLabel->setText("—");
        m_subDelayValLabel->setText("—");
        m_mainsHpValLabel->setText("—");
        m_subLpValLabel->setText("—");
        m_confidenceValLabel->setText("—");
        m_summaryLabel->setText(
            "Select mains and subwoofer measurements above and click Recommend to compute crossover settings.");
        m_applyDelayBtn->setEnabled(false);
    }
}
