#include "ui/ConvolutionImportDlg.h"

#include "models/ConvCoefficientLoader.h"
#include "ui/StyleTheme.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUuid>
#include <QVBoxLayout>
#include <set>
#include <stdexcept>

ConvolutionImportDlg::ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Import Impulse Responses");
    resize(480, 580);
    setupUi();
}

void ConvolutionImportDlg::setupUi() {
    auto outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // Header Block
    auto headerWidget = new QWidget(this);
    auto headerBox = new QVBoxLayout(headerWidget);
    headerBox->setContentsMargins(16, 16, 16, 16);
    headerBox->setSpacing(4);

    auto titleLbl = new QLabel("Import Impulse Responses", headerWidget);
    titleLbl->setFont(QFont("sans-serif", 13, QFont::Bold));
    headerBox->addWidget(titleLbl);

    auto subtitleLbl = new QLabel("Import files as a unified multi-rate Convolution Preset.", headerWidget);
    subtitleLbl->setStyleSheet(QString("color: %1; font-size: 11px;").arg(StyleTheme::textSecondary().name()));
    headerBox->addWidget(subtitleLbl);
    outerLayout->addWidget(headerWidget);

    auto topDivider = new QFrame(this);
    topDivider->setFrameShape(QFrame::HLine);
    topDivider->setFrameShadow(QFrame::Sunken);
    outerLayout->addWidget(topDivider);

    // Scrollable Central Area
    auto scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto contentWidget = new QWidget(scrollArea);
    auto contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(20);

    // GroupBox "Preset Details"
    auto detailsGroup = new QGroupBox("Preset Details", contentWidget);
    auto form = new QFormLayout(detailsGroup);
    form->setContentsMargins(12, 16, 12, 12);
    form->setSpacing(12);

    m_nameEdit = new QLineEdit(detailsGroup);
    m_nameEdit->setPlaceholderText("e.g., My Custom IR");
    connect(m_nameEdit, &QLineEdit::textChanged, this, &ConvolutionImportDlg::updateItemsList);
    form->addRow("Preset Name", m_nameEdit);

    m_kindEdit = new QLineEdit(detailsGroup);
    m_kindEdit->setText("Imported");
    m_kindEdit->setPlaceholderText("e.g., Imported, Min-phase");
    form->addRow("Kind Label", m_kindEdit);

    contentLayout->addWidget(detailsGroup);

    // Impulse Response Files Section
    auto fileSectionLayout = new QVBoxLayout();
    fileSectionLayout->setSpacing(8);

    auto tableHeader = new QHBoxLayout();
    auto filesLbl = new QLabel("Impulse Response Files", contentWidget);
    filesLbl->setFont(QFont("sans-serif", 12, QFont::Bold));
    tableHeader->addWidget(filesLbl);
    tableHeader->addStretch();

    auto addBtn = new QPushButton("+ Add File(s)…", contentWidget);
    connect(addBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onAddFilesClicked);
    tableHeader->addWidget(addBtn);
    fileSectionLayout->addLayout(tableHeader);

    // Empty state container
    m_emptyStateWidget = new QWidget(contentWidget);
    m_emptyStateWidget->setStyleSheet(
        QString("QWidget { border: 1px dashed %1; border-radius: 8px; background: transparent; }"
                "QLabel { border: none; }")
            .arg(StyleTheme::border().name()));
    auto emptyLayout = new QVBoxLayout(m_emptyStateWidget);
    emptyLayout->setContentsMargins(20, 40, 20, 40);
    emptyLayout->setSpacing(12);

    auto emptyIcon = new QLabel("⇣", m_emptyStateWidget);
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyIcon->setFont(QFont("sans-serif", 24));
    emptyIcon->setStyleSheet(QString("color: %1;").arg(StyleTheme::textSecondary().name()));
    emptyLayout->addWidget(emptyIcon);

    auto emptyText = new QLabel("No files selected. Click 'Add File(s)' to begin.", m_emptyStateWidget);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setStyleSheet(QString("color: %1; font-size: 13px;").arg(StyleTheme::textSecondary().name()));
    emptyLayout->addWidget(emptyText);

    fileSectionLayout->addWidget(m_emptyStateWidget);

    // Item Cards layout
    m_itemListLayout = new QVBoxLayout();
    m_itemListLayout->setSpacing(12);
    fileSectionLayout->addLayout(m_itemListLayout);

    // Warning label for duplicate rates
    m_warningLabel = new QLabel(this);
    m_warningLabel->setText(
        "⚠️ Duplicate sample rates found. Each file in the preset must represent a different sample rate.");
    m_warningLabel->setStyleSheet(QString("color: %1; font-size: 11px;").arg(StyleTheme::accentOrange().name()));
    m_warningLabel->setVisible(false);
    m_warningLabel->setWordWrap(true);
    fileSectionLayout->addWidget(m_warningLabel);

    contentLayout->addLayout(fileSectionLayout);

    // Error banner container
    m_errorWidget = new QWidget(contentWidget);
    m_errorWidget->setStyleSheet(QString("QWidget { background-color: rgba(255, 59, 48, 0.1); border-radius: 8px; }"
                                         "QLabel { background: transparent; color: %1; font-size: 13px; }")
                                     .arg(StyleTheme::accentRed().name()));
    auto errorLayout = new QHBoxLayout(m_errorWidget);
    errorLayout->setContentsMargins(12, 12, 12, 12);

    auto errIcon = new QLabel("🛑", m_errorWidget);
    errorLayout->addWidget(errIcon);

    m_errorLabel = new QLabel(m_errorWidget);
    m_errorLabel->setWordWrap(true);
    errorLayout->addWidget(m_errorLabel, 1);

    m_errorWidget->setVisible(false);
    contentLayout->addWidget(m_errorWidget);

    contentLayout->addStretch();
    scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(scrollArea);

    auto bottomDivider = new QFrame(this);
    bottomDivider->setFrameShape(QFrame::HLine);
    bottomDivider->setFrameShadow(QFrame::Sunken);
    outerLayout->addWidget(bottomDivider);

    // Footer Buttons
    auto footerWidget = new QWidget(this);
    auto btnLayout = new QHBoxLayout(footerWidget);
    btnLayout->setContentsMargins(16, 12, 16, 12);
    btnLayout->setSpacing(12);
    btnLayout->addStretch();

    auto cancelBtn = new QPushButton("Cancel", footerWidget);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    m_importBtn = new QPushButton("Import", footerWidget);
    m_importBtn->setDefault(true);
    connect(m_importBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onImportClicked);
    btnLayout->addWidget(m_importBtn);

    outerLayout->addWidget(footerWidget);

    updateItemsList();
}

bool ConvolutionImportDlg::hasDuplicateRates() const {
    std::set<int> rates;
    for (const auto& item : m_items) {
        if (rates.count(item.sampleRate))
            return true;
        rates.insert(item.sampleRate);
    }
    return false;
}

void ConvolutionImportDlg::updateItemsList() {
    QLayoutItem* child;
    while ((child = m_itemListLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            delete child->widget();
        }
        delete child;
    }

    if (m_items.empty()) {
        m_emptyStateWidget->setVisible(true);
    } else {
        m_emptyStateWidget->setVisible(false);

        static const std::vector<int> standardRates = {32000,  44100,  48000,  88200,  96000, 176400,
                                                       192000, 352800, 384000, 705600, 768000};
        static const QStringList formats = {"WAV", "FLOAT64", "FLOAT32", "S16_LE", "S32_LE", "TEXT"};

        for (size_t i = 0; i < m_items.size(); ++i) {
            auto card = new QWidget(this);
            card->setStyleSheet(QString("QWidget { background-color: %1; border: 1px solid %2; border-radius: 8px; }"
                                        "QLabel, QComboBox, QSpinBox, QPushButton { border-radius: 4px; }")
                                    .arg(StyleTheme::cardBg().name())
                                    .arg(StyleTheme::border().name()));

            auto cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(12, 12, 12, 12);
            cardLayout->setSpacing(8);

            // Card Header
            auto topRow = new QHBoxLayout();
            QFileInfo fi(m_items[i].filePath);

            auto iconLbl = new QLabel(m_items[i].format == "WAV" ? "〰" : "📄", card);
            iconLbl->setStyleSheet(
                QString("color: %1; font-size: 14px; border: none;").arg(StyleTheme::accent().name()));
            topRow->addWidget(iconLbl);

            auto nameLbl = new QLabel(fi.fileName(), card);
            nameLbl->setFont(QFont("sans-serif", 11, QFont::Bold));
            nameLbl->setStyleSheet("border: none;");
            topRow->addWidget(nameLbl, 1);

            auto delBtn = new QPushButton("🗑", card);
            delBtn->setFlat(true);
            delBtn->setStyleSheet("color: #ff3b30; font-size: 14px; border: none; background: transparent;");
            connect(delBtn, &QPushButton::clicked, [this, i]() {
                m_items.erase(m_items.begin() + i);
                updateItemsList();
            });
            topRow->addWidget(delBtn);

            cardLayout->addLayout(topRow);

            // Details Grid
            auto grid = new QGridLayout();
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setSpacing(8);

            // Sample Rate
            auto rateLbl = new QLabel("Sample Rate", card);
            rateLbl->setStyleSheet(
                QString("color: %1; font-size: 11px; border: none;").arg(StyleTheme::textSecondary().name()));
            grid->addWidget(rateLbl, 0, 0);

            auto rateCombo = new QComboBox(card);
            for (int r : standardRates) {
                rateCombo->addItem(QString("%1 Hz").arg(r), r);
            }
            int rateIdx = rateCombo->findData(m_items[i].sampleRate);
            if (rateIdx >= 0)
                rateCombo->setCurrentIndex(rateIdx);
            rateCombo->setFixedWidth(140);
            connect(rateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, rateCombo]() {
                m_items[i].sampleRate = rateCombo->currentData().toInt();
                m_warningLabel->setVisible(hasDuplicateRates());
                bool disabled =
                    m_items.empty() || m_nameEdit->text().trimmed().isEmpty() || hasDuplicateRates() || m_isImporting;
                m_importBtn->setEnabled(!disabled);
            });
            grid->addWidget(rateCombo, 0, 1);

            // Format
            auto fmtLbl = new QLabel("Format", card);
            fmtLbl->setStyleSheet(
                QString("color: %1; font-size: 11px; border: none;").arg(StyleTheme::textSecondary().name()));
            grid->addWidget(fmtLbl, 1, 0);

            auto fmtCombo = new QComboBox(card);
            fmtCombo->addItems(formats);
            fmtCombo->setCurrentText(m_items[i].format);
            fmtCombo->setFixedWidth(140);
            connect(fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, fmtCombo]() {
                m_items[i].format = fmtCombo->currentText();
                updateItemsList();
            });
            grid->addWidget(fmtCombo, 1, 1);

            // WAV Channel (if WAV)
            if (m_items[i].format == "WAV") {
                auto chLbl = new QLabel("WAV Channel", card);
                chLbl->setStyleSheet(
                    QString("color: %1; font-size: 11px; border: none;").arg(StyleTheme::textSecondary().name()));
                grid->addWidget(chLbl, 2, 0);

                auto chSpin = new QSpinBox(card);
                chSpin->setRange(0, 15);
                chSpin->setPrefix("Channel ");
                chSpin->setValue(m_items[i].channel);
                chSpin->setFixedWidth(140);
                connect(chSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        [this, i](int val) { m_items[i].channel = val; });
                grid->addWidget(chSpin, 2, 1);
            }

            cardLayout->addLayout(grid);
            m_itemListLayout->addWidget(card);
        }
    }

    m_warningLabel->setVisible(hasDuplicateRates());

    bool disabled = m_items.empty() || m_nameEdit->text().trimmed().isEmpty() || hasDuplicateRates() || m_isImporting;
    m_importBtn->setEnabled(!disabled);
}

void ConvolutionImportDlg::onAddFilesClicked() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, "Select IR File(s)", "", "Audio Files (*.wav *.f64 *.f32 *.pcm *.txt *.data);;All Files (*)");

    static const std::vector<int> allRates = {768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200,
                                              48000,  44100,  32000,  22050,  16000,  11025,  8000};

    for (const auto& file : files) {
        ImportItem item;
        item.filePath = file;
        QFileInfo fi(file);

        auto wavInfo = ConvCoefficientLoader::parseWavHeader(file.toStdString());
        if (wavInfo.has_value()) {
            item.sampleRate = wavInfo->sampleRate;
            item.format = "WAV";
        } else {
            QString lower = file.toLower();
            if (lower.endsWith(".txt"))
                item.format = "TEXT";
            else if (lower.endsWith(".f32") || lower.contains("float32") || lower.contains("f32"))
                item.format = "FLOAT32";
            else if (lower.endsWith(".f64") || lower.contains("float64") || lower.contains("f64"))
                item.format = "FLOAT64";
            else if (lower.contains("s16"))
                item.format = "S16_LE";
            else if (lower.contains("s32"))
                item.format = "S32_LE";
            else
                item.format = "FLOAT64";

            for (int r : allRates) {
                if (lower.contains(QString::number(r)) || lower.contains(QString("%1k").arg(r / 1000))) {
                    item.sampleRate = r;
                    break;
                }
            }
        }
        m_items.push_back(item);

        if (m_nameEdit->text().isEmpty()) {
            QString base = fi.baseName();
            base.remove(QRegularExpression("[-_]\\d+Hz|[-_]\\d+k", QRegularExpression::CaseInsensitiveOption));
            m_nameEdit->setText(base);
        }
    }
    updateItemsList();
}

void ConvolutionImportDlg::onImportClicked() {
    if (m_items.empty())
        return;

    m_isImporting = true;
    m_importBtn->setEnabled(false);
    m_errorWidget->setVisible(false);

    try {
        QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir irDir(appDataDir + "/IRs");
        if (!irDir.exists())
            irDir.mkpath(".");

        QUuid presetId = QUuid::createUuid();
        std::map<int, std::string> paths;
        int firstCoeffCount = 0;

        for (const auto& item : m_items) {
            auto coeffs = ConvCoefficientLoader::loadCoefficients(
                item.filePath.toStdString(), item.format.toStdString(), item.channel, item.sampleRate);

            if (coeffs.empty()) {
                throw std::runtime_error(QString("File '%1' contains zero coefficients.")
                                             .arg(QFileInfo(item.filePath).fileName())
                                             .toStdString());
            }

            if (firstCoeffCount == 0) {
                firstCoeffCount = static_cast<int>(coeffs.size());
            }

            QString destFileName =
                QString("Imported-%1-%2.f64").arg(presetId.toString(QUuid::WithoutBraces).left(8)).arg(item.sampleRate);
            QString destPath = irDir.filePath(destFileName);

            ConvCoefficientLoader::saveRawFloat64(coeffs, destPath.toStdString());
            paths[item.sampleRate] = destPath.toStdString();
        }

        std::string name = m_nameEdit->text().trimmed().toStdString();
        std::string kind = m_kindEdit->text().trimmed().toStdString();
        if (kind.empty())
            kind = "Imported";

        ConvolutionPreset preset(name, paths, firstCoeffCount, kind);
        m_pipeline->addConvPreset(preset);

        accept();
    } catch (const std::exception& ex) {
        m_errorLabel->setText(ex.what());
        m_errorWidget->setVisible(true);
        m_isImporting = false;
        m_importBtn->setEnabled(true);
    }
}
