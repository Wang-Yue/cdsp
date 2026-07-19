#include "ui/SubwooferAssistDlg.h"

#include "ui/StyleTheme.h"

#include <QFrame>

SubwooferAssistDlg::SubwooferAssistDlg(MeasurementSession* session, QWidget* parent)
    : QDialog(parent), m_session(session) {
    setWindowTitle("Subwoofer Crossover Assist");
    setFixedWidth(480);
    setStyleSheet("QDialog { background-color: " + (StyleTheme::isDark() ? QString("#1e1e1e") : QString("#f6f6f6")) +
                  "; }");

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Header bar
    auto headerLayout = new QHBoxLayout();
    auto titleLabel = new QLabel("🔊 Subwoofer Crossover Assist", this);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 13px; color: " +
                              (StyleTheme::isDark() ? QString("#eee") : QString("#222")) + ";");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch(1);

    auto recommendBtn = new QPushButton("✨ Recommend", this);
    recommendBtn->setStyleSheet("QPushButton { padding: 4px 10px; font-weight: bold; font-size: 11px; }");
    connect(recommendBtn, &QPushButton::clicked, this, &SubwooferAssistDlg::onRecommendClicked);
    headerLayout->addWidget(recommendBtn);

    mainLayout->addLayout(headerLayout);

    auto line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line);

    // Results container
    m_resultsWidget = new QWidget(this);
    auto resultsLayout = new QVBoxLayout(m_resultsWidget);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(10);

    auto metaRow = new QHBoxLayout();
    metaRow->setSpacing(12);

    metaRow->addWidget(createMetaCell("Crossover", &m_crossoverValLabel));
    metaRow->addWidget(createMetaCell("Sub delay", &m_subDelayValLabel));
    metaRow->addWidget(createMetaCell("Mains HP", &m_mainsHpValLabel));
    metaRow->addWidget(createMetaCell("Sub LP", &m_subLpValLabel));
    metaRow->addWidget(createMetaCell("Confidence", &m_confidenceValLabel));
    metaRow->addStretch(1);

    resultsLayout->addLayout(metaRow);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setStyleSheet("font-size: 11px; color: #888;");
    resultsLayout->addWidget(m_summaryLabel);

    mainLayout->addWidget(m_resultsWidget);

    updateUi();
}

QWidget* SubwooferAssistDlg::createMetaCell(const QString& title, QLabel** valueLabelOut) {
    auto cell = new QWidget(this);
    auto layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto titleLbl = new QLabel(title, cell);
    titleLbl->setStyleSheet("font-size: 10px; color: #888;");
    layout->addWidget(titleLbl);

    auto valLbl = new QLabel("—", cell);
    valLbl->setStyleSheet("font-family: monospace; font-weight: bold; font-size: 11px;");
    layout->addWidget(valLbl);

    if (valueLabelOut)
        *valueLabelOut = valLbl;
    return cell;
}

void SubwooferAssistDlg::onRecommendClicked() {
    if (m_session) {
        m_recommendation = m_session->computeSubwooferRecommendation();
        updateUi();
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
    } else {
        m_crossoverValLabel->setText("—");
        m_subDelayValLabel->setText("—");
        m_mainsHpValLabel->setText("—");
        m_subLpValLabel->setText("—");
        m_confidenceValLabel->setText("—");
        m_summaryLabel->setText("Click Recommend to compute crossover settings from the most recent mains-only and "
                                "subwoofer-only measurements.");
    }
}
