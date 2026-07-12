#include "ui/ConvolutionImportDlg.h"

#include "models/ConvCoefficientLoader.h"
#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <set>

ConvolutionImportDlg::ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Import Impulse Responses");
    resize(480, 580);
    setupUi();
}

void ConvolutionImportDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(14);

    auto headerBox = new QVBoxLayout();
    auto titleLbl = new QLabel("Import Impulse Responses", this);
    titleLbl->setFont(QFont("sans-serif", 13, QFont::Bold));
    headerBox->addWidget(titleLbl);

    auto subtitleLbl = new QLabel("Import files as a unified multi-rate Convolution Preset.", this);
    subtitleLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    headerBox->addWidget(subtitleLbl);
    mainLayout->addLayout(headerBox);

    auto form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("e.g., My Custom IR");
    form->addRow("Preset Name:", m_nameEdit);

    m_kindEdit = new QLineEdit(this);
    m_kindEdit->setText("Imported");
    m_kindEdit->setPlaceholderText("e.g., Imported, Min-phase");
    form->addRow("Kind Label:", m_kindEdit);

    m_normalizeCheck = new QCheckBox("Normalize IR Peak (0 dBFS)", this);
    m_normalizeCheck->setChecked(true);
    form->addRow("Normalization:", m_normalizeCheck);

    m_delayCompSpin = new QDoubleSpinBox(this);
    m_delayCompSpin->setRange(0.0, 1000.0);
    m_delayCompSpin->setSuffix(" ms");
    form->addRow("Delay Compensation:", m_delayCompSpin);

    mainLayout->addLayout(form);

    auto tableHeader = new QHBoxLayout();
    tableHeader->addWidget(new QLabel("Impulse Response Files", this));
    tableHeader->addStretch();

    auto addBtn = new QPushButton("Add File(s)...", this);
    connect(addBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onAddFilesClicked);
    tableHeader->addWidget(addBtn);
    mainLayout->addLayout(tableHeader);

    m_fileTable = new QTableWidget(0, 5, this);
    m_fileTable->setHorizontalHeaderLabels({"File", "Sample Rate", "Format", "Channel", ""});
    m_fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_fileTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    mainLayout->addWidget(m_fileTable);

    m_irPlotPreview = new ConvolutionIRPlot(this);
    m_irPlotPreview->setFixedHeight(100);
    mainLayout->addWidget(m_irPlotPreview);

    m_warningLabel = new QLabel(this);
    m_warningLabel->setStyleSheet("color: #ff9500; font-size: 11px;");
    m_warningLabel->setVisible(false);
    mainLayout->addWidget(m_warningLabel);

    m_infoLabel = new QLabel(
        "Add IR files to build a multi-rate convolution preset with peak normalization and latency compensation.",
        this);
    m_infoLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    mainLayout->addWidget(m_infoLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto cancelBtn = new QPushButton("Cancel", this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    m_importBtn = new QPushButton("Import", this);
    m_importBtn->setDefault(true);
    connect(m_importBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onImportClicked);
    btnLayout->addWidget(m_importBtn);

    mainLayout->addLayout(btnLayout);
    updateTable();
}

void ConvolutionImportDlg::onAddFilesClicked() {
    QStringList files =
        QFileDialog::getOpenFileNames(this, "Select IR File(s)", "", "Audio Files (*.wav *.f64 *.f32 *.pcm *.txt)");
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
            else if (lower.endsWith(".f32") || lower.contains("float32"))
                item.format = "FLOAT32";
            else if (lower.endsWith(".f64") || lower.contains("float64"))
                item.format = "FLOAT64";
            else if (lower.contains("s16"))
                item.format = "S16_LE";
            else if (lower.contains("s32"))
                item.format = "S32_LE";
            else
                item.format = "FLOAT64";

            static const std::vector<int> stdRates = {768000, 705600, 384000, 352800, 192000, 176400,
                                                      96000,  88200,  48000,  44100,  32000};
            for (int r : stdRates) {
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
    updateTable();
}

void ConvolutionImportDlg::updateTable() {
    m_fileTable->setRowCount(0);
    std::set<int> rates;
    bool duplicate = false;

    for (size_t i = 0; i < m_items.size(); ++i) {
        int row = m_fileTable->rowCount();
        m_fileTable->insertRow(row);
        auto& item = m_items[i];

        QFileInfo fi(item.filePath);
        m_fileTable->setItem(row, 0, new QTableWidgetItem(fi.fileName()));

        auto rateCombo = new QComboBox(this);
        for (int r : {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}) {
            rateCombo->addItem(QString("%1 Hz").arg(r), r);
        }
        rateCombo->setCurrentText(QString("%1 Hz").arg(item.sampleRate));
        connect(rateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, rateCombo]() {
            m_items[i].sampleRate = rateCombo->currentData().toInt();
            updateTable();
        });
        m_fileTable->setCellWidget(row, 1, rateCombo);

        if (rates.count(item.sampleRate))
            duplicate = true;
        rates.insert(item.sampleRate);

        auto fmtCombo = new QComboBox(this);
        fmtCombo->addItems({"WAV", "FLOAT64", "FLOAT32", "S16_LE", "S32_LE", "TEXT"});
        fmtCombo->setCurrentText(item.format);
        connect(fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, fmtCombo]() {
            m_items[i].format = fmtCombo->currentText();
            updateTable();
        });
        m_fileTable->setCellWidget(row, 2, fmtCombo);

        auto chSpin = new QSpinBox(this);
        chSpin->setRange(0, 15);
        chSpin->setValue(item.channel);
        chSpin->setEnabled(item.format == "WAV");
        connect(chSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this, i](int val) { m_items[i].channel = val; });
        m_fileTable->setCellWidget(row, 3, chSpin);

        auto delBtn = new QPushButton("X", this);
        delBtn->setFixedWidth(30);
        connect(delBtn, &QPushButton::clicked, [this, i]() {
            m_items.erase(m_items.begin() + i);
            updateTable();
        });
        m_fileTable->setCellWidget(row, 4, delBtn);
    }

    if (!m_items.empty()) {
        m_irPlotPreview->setIRPath(m_items[0].filePath.toStdString(), m_items[0].filePath.toStdString());
        m_irPlotPreview->setVisible(true);
    } else {
        m_irPlotPreview->setVisible(false);
    }

    if (duplicate) {
        m_warningLabel->setText(
            "⚠️ Duplicate sample rates found. Each file in the preset must represent a different sample rate.");
        m_warningLabel->setVisible(true);
    } else {
        m_warningLabel->setVisible(false);
    }

    m_importBtn->setEnabled(!m_items.empty() && !duplicate && !m_nameEdit->text().trimmed().isEmpty());
}

#include <QDir>
#include <QStandardPaths>
#include <QUuid>

void ConvolutionImportDlg::onImportClicked() {
    if (m_items.empty())
        return;

    QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir irDir(appDataDir + "/IRs");
    if (!irDir.exists())
        irDir.mkpath(".");

    QUuid presetId = QUuid::createUuid();
    std::map<int, std::string> paths;
    int firstCoeffCount = 0;
    bool doNormalize = m_normalizeCheck->isChecked();
    double delayMs = m_delayCompSpin->value();

    for (const auto& item : m_items) {
        auto coeffs = ConvCoefficientLoader::loadCoefficients(item.filePath.toStdString(), item.format.toStdString(),
                                                              item.channel, item.sampleRate);

        if (doNormalize && !coeffs.empty()) {
            double maxVal = 0.0;
            for (double c : coeffs) {
                double absVal = std::abs(c);
                if (absVal > maxVal)
                    maxVal = absVal;
            }
            double targetDb = 0.0;
            double scale = maxVal > 0.0 ? std::pow(10.0, targetDb / 20.0) / maxVal : 1.0;
            for (double& c : coeffs)
                c *= scale;
        }

        if (delayMs > 0.0) {
            int padSamples = qRound((delayMs / 1000.0) * item.sampleRate);
            if (padSamples > 0) {
                coeffs.insert(coeffs.begin(), padSamples, 0.0);
            }
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

    std::string name = m_nameEdit->text().toStdString();
    std::string kind = m_kindEdit->text().toStdString();
    if (kind.empty())
        kind = "Imported";

    ConvolutionPreset preset(name, paths, firstCoeffCount, kind);
    m_pipeline->addConvPreset(preset);

    QMessageBox::information(this, "Success",
                             QString("Imported convolution preset '%1' with %2 rate file(s).")
                                 .arg(QString::fromStdString(name))
                                 .arg(paths.size()));
    accept();
}
