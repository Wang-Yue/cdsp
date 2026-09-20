#include "ui/ConvolutionImportDlg.h"

#include "config/DSPConfigTypes.h"        // for MONITOR_STANDARD_RATES
#include "models/ConvCoefficientLoader.h" // for WavHeaderInfo, ConvCoefficientLoader
#include "models/ConvolutionPreset.h"     // for ConvolutionPreset

#include <QCheckBox>          // for QCheckBox
#include <QComboBox>          // for QComboBox
#include <QDialogButtonBox>   // for QDialogButtonBox, operator|
#include <QDir>               // for QDir
#include <QDragEnterEvent>    // for QDragEnterEvent
#include <QDropEvent>         // for QDropEvent
#include <QFileDialog>        // for QFileDialog
#include <QFileInfo>          // for QFileInfo
#include <QFont>              // for QFont
#include <QFormLayout>        // for QFormLayout
#include <QFrame>             // for QFrame
#include <QGridLayout>        // for QGridLayout
#include <QGroupBox>          // for QGroupBox
#include <QHBoxLayout>        // for QHBoxLayout
#include <QLabel>             // for QLabel
#include <QLayoutItem>        // for QLayoutItem
#include <QLineEdit>          // for QLineEdit
#include <QList>              // for QList
#include <QMetaObject>        // for QMetaObject
#include <QMimeData>          // for QMimeData
#include <QPushButton>        // for QPushButton
#include <QRegularExpression> // for QRegularExpression
#include <QScrollArea>        // for QScrollArea
#include <QSpinBox>           // for QSpinBox
#include <QStandardPaths>     // for QStandardPaths
#include <QUrl>               // for QUrl
#include <QUuid>              // for QUuid
#include <QVBoxLayout>        // for QVBoxLayout
#include <QVariant>           // for QVariant
#include <Qt>                 // for AlignmentFlag, ConnectionType
#include <QtConcurrent>       // for run
#include <QtGlobal>           // for QOverload
#include <algorithm>          // for sort
#include <exception>          // for exception
#include <functional>         // for greater
#include <optional>           // for optional
#include <set>                // for set
#include <stddef.h>           // for size_t

ConvolutionImportDlg::ConvolutionImportDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Import Impulse Responses");
    resize(480, 580);
    setAcceptDrops(true);
    setupUi();
    connect(&m_watcher, &QFutureWatcher<ImportResult>::finished, this, &ConvolutionImportDlg::onImportFinished);
}

ConvolutionImportDlg::~ConvolutionImportDlg() {
    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        m_watcher.waitForFinished();
    }
}

void ConvolutionImportDlg::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void ConvolutionImportDlg::dropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    if (!mimeData->hasUrls())
        return;

    QStringList files;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) {
            QString path = url.toLocalFile();
            QFileInfo fi(path);
            if (fi.isFile()) {
                QString ext = fi.suffix().toLower();
                if (ext == "wav" || ext == "f64" || ext == "f32" || ext == "pcm" || ext == "txt" || ext == "data" ||
                    ext == "s16" || ext == "s32") {
                    files.append(path);
                }
            }
        }
    }

    if (!files.isEmpty()) {
        addImportFiles(files);
        event->acceptProposedAction();
    }
}

void ConvolutionImportDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);

    auto subtitleLbl = new QLabel("Import files as a unified multi-rate Convolution Preset.", this);
    subtitleLbl->setWordWrap(true);
    mainLayout->addWidget(subtitleLbl);

    // Scrollable Central Area
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    auto contentWidget = new QWidget(m_scrollArea);
    auto contentLayout = new QVBoxLayout(contentWidget);

    // GroupBox "Preset Details"
    auto detailsGroup = new QGroupBox("Preset Details", contentWidget);
    auto form = new QFormLayout(detailsGroup);

    m_nameEdit = new QLineEdit(detailsGroup);
    m_nameEdit->setPlaceholderText("e.g., My Custom IR");
    connect(m_nameEdit, &QLineEdit::textChanged, this, &ConvolutionImportDlg::updateImportButtonState);
    form->addRow("Preset Name", m_nameEdit);

    m_kindEdit = new QLineEdit(detailsGroup);
    m_kindEdit->setText("Imported");
    m_kindEdit->setPlaceholderText("e.g., Imported, Min-phase");
    form->addRow("Kind Label", m_kindEdit);

    contentLayout->addWidget(detailsGroup);

    // Impulse Response Files Section
    auto fileSectionLayout = new QVBoxLayout();

    auto tableHeader = new QHBoxLayout();
    auto filesLbl = new QLabel("Impulse Response Files", contentWidget);
    QFont f = font();
    f.setBold(true);
    filesLbl->setFont(f);
    tableHeader->addWidget(filesLbl);
    tableHeader->addStretch();

    auto addBtn = new QPushButton("Add File(s)…", contentWidget);
    connect(addBtn, &QPushButton::clicked, this, &ConvolutionImportDlg::onAddFilesClicked);
    tableHeader->addWidget(addBtn);
    fileSectionLayout->addLayout(tableHeader);

    // Empty state container
    m_emptyStateWidget = new QFrame(contentWidget);
    m_emptyStateWidget->setFrameShape(QFrame::StyledPanel);
    auto emptyLayout = new QVBoxLayout(m_emptyStateWidget);

    auto emptyIcon = new QLabel("⇣", m_emptyStateWidget);
    emptyIcon->setAlignment(Qt::AlignCenter);
    QFont iconF = font();
    iconF.setPointSize(24);
    emptyIcon->setFont(iconF);
    emptyLayout->addWidget(emptyIcon);

    auto emptyText =
        new QLabel("No files selected. Click 'Add File(s)' or drag & drop files to begin.", m_emptyStateWidget);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyText);

    fileSectionLayout->addWidget(m_emptyStateWidget);

    // Item Cards layout
    m_itemListLayout = new QVBoxLayout();
    fileSectionLayout->addLayout(m_itemListLayout);

    // Warning label for duplicate rates
    m_warningLabel = new QLabel(contentWidget);
    m_warningLabel->setText(
        "⚠️ Duplicate sample rates found. Each file in the preset must represent a different sample rate.");
    m_warningLabel->setVisible(false);
    m_warningLabel->setWordWrap(true);
    fileSectionLayout->addWidget(m_warningLabel);

    contentLayout->addLayout(fileSectionLayout);

    // Error banner container
    m_errorWidget = new QFrame(contentWidget);
    m_errorWidget->setFrameShape(QFrame::StyledPanel);
    auto errorLayout = new QHBoxLayout(m_errorWidget);

    auto errIcon = new QLabel("🛑", m_errorWidget);
    errorLayout->addWidget(errIcon);

    m_errorLabel = new QLabel(m_errorWidget);
    m_errorLabel->setWordWrap(true);
    errorLayout->addWidget(m_errorLabel, 1);

    m_errorWidget->setVisible(false);
    contentLayout->addWidget(m_errorWidget);

    contentLayout->addStretch();
    m_scrollArea->setWidget(contentWidget);
    mainLayout->addWidget(m_scrollArea);

    // Standard Dialog Button Box
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_importBtn = m_buttonBox->button(QDialogButtonBox::Ok);
    if (m_importBtn) {
        m_importBtn->setText("Import");
        m_importBtn->setDefault(true);
    }
    m_cancelBtn = m_buttonBox->button(QDialogButtonBox::Cancel);

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &ConvolutionImportDlg::onImportClicked);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    mainLayout->addWidget(m_buttonBox);

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
            child->widget()->deleteLater();
        }
        delete child;
    }

    if (m_items.empty()) {
        m_emptyStateWidget->setVisible(true);
    } else {
        m_emptyStateWidget->setVisible(false);

        std::vector<int> standardRates = MONITOR_STANDARD_RATES;
        static const QStringList formats = {"WAV", "FLOAT64", "FLOAT32", "S16_LE", "S32_LE", "TEXT"};

        for (size_t i = 0; i < m_items.size(); ++i) {
            auto card = new QFrame(this);
            card->setFrameShape(QFrame::StyledPanel);

            auto cardLayout = new QVBoxLayout(card);

            // Card Header
            auto topRow = new QHBoxLayout();
            QFileInfo fi(m_items[i].filePath);

            auto iconLbl = new QLabel(m_items[i].format == "WAV" ? "〰" : "📄", card);
            topRow->addWidget(iconLbl);

            auto nameLbl = new QLabel(fi.fileName(), card);
            QFont nameF = font();
            nameF.setBold(true);
            nameLbl->setFont(nameF);
            topRow->addWidget(nameLbl, 1);

            auto delBtn = new QPushButton("✕", card);
            delBtn->setToolTip("Remove this file");
            delBtn->setFlat(true);
            connect(delBtn, &QPushButton::clicked, [this, i]() {
                m_items.erase(m_items.begin() + i);
                QMetaObject::invokeMethod(this, [this]() { updateItemsList(); }, Qt::QueuedConnection);
            });
            topRow->addWidget(delBtn);

            cardLayout->addLayout(topRow);

            // Details Grid
            auto grid = new QGridLayout();

            // Sample Rate
            auto rateLbl = new QLabel("Sample Rate", card);
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
                updateImportButtonState();
            });
            grid->addWidget(rateCombo, 0, 1);

            // Format
            auto fmtLbl = new QLabel("Format", card);
            grid->addWidget(fmtLbl, 1, 0);

            auto fmtCombo = new QComboBox(card);
            fmtCombo->addItems(formats);
            fmtCombo->setCurrentText(m_items[i].format);
            fmtCombo->setFixedWidth(140);
            connect(fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, i, fmtCombo]() {
                m_items[i].format = fmtCombo->currentText();
                QMetaObject::invokeMethod(this, [this]() { updateItemsList(); }, Qt::QueuedConnection);
            });
            grid->addWidget(fmtCombo, 1, 1);

            int nextRow = 2;
            // WAV Channel (if WAV)
            if (m_items[i].format == "WAV") {
                auto chLbl = new QLabel("WAV Channel", card);
                grid->addWidget(chLbl, nextRow, 0);

                auto chSpin = new QSpinBox(card);
                chSpin->setRange(0, 15);
                chSpin->setPrefix("Channel ");
                chSpin->setValue(m_items[i].channel);
                chSpin->setFixedWidth(140);
                connect(chSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        [this, i](int val) { m_items[i].channel = val; });
                grid->addWidget(chSpin, nextRow, 1);
                nextRow++;
            }

            // Phase Invert Checkbox
            auto phaseCheck = new QCheckBox("Invert Phase", card);
            phaseCheck->setChecked(m_items[i].invertPhase);
            connect(phaseCheck, &QCheckBox::toggled, [this, i](bool checked) { m_items[i].invertPhase = checked; });
            grid->addWidget(phaseCheck, nextRow, 0, 1, 2);

            cardLayout->addLayout(grid);
            m_itemListLayout->addWidget(card);
        }
    }

    updateImportButtonState();
}

void ConvolutionImportDlg::updateImportButtonState() {
    m_warningLabel->setVisible(hasDuplicateRates());
    bool disabled = m_items.empty() || m_nameEdit->text().trimmed().isEmpty() || hasDuplicateRates() || m_isImporting;
    m_importBtn->setEnabled(!disabled);
}

void ConvolutionImportDlg::addImportFiles(const QStringList& files) {
    std::vector<int> allRates = MONITOR_STANDARD_RATES;
    std::sort(allRates.begin(), allRates.end(), std::greater<int>());

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
                QString rateStr = QString::number(r);
                QString kStr = QString("%1k").arg(r / 1000);
                QString doubleKStr = QString("%1k").arg(static_cast<double>(r) / 1000.0, 0, 'g', 4);

                if (lower.contains(rateStr) || lower.contains(kStr) || lower.contains(doubleKStr)) {
                    item.sampleRate = r;
                    break;
                }
            }
        }
        m_items.push_back(item);
    }

    if (m_nameEdit->text().trimmed().isEmpty() && !m_items.empty()) {
        QFileInfo firstFi(m_items.front().filePath);
        QString base = firstFi.completeBaseName();
        base.remove(QRegularExpression("-\\d+Hz|_\\d+", QRegularExpression::CaseInsensitiveOption));
        m_nameEdit->setText(base);
    }

    updateItemsList();
}

void ConvolutionImportDlg::onAddFilesClicked() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, "Select IR File(s)", "", "Audio Files (*.wav *.f64 *.f32 *.pcm *.txt *.data);;All Files (*)");
    if (!files.isEmpty()) {
        addImportFiles(files);
    }
}

void ConvolutionImportDlg::onImportClicked() {
    if (m_items.empty())
        return;

    m_isImporting = true;
    m_importBtn->setText("Importing...");
    m_importBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    m_scrollArea->setEnabled(false);
    m_errorWidget->setVisible(false);

    QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir irDir(appDataDir + "/IRs");
    if (!irDir.exists())
        irDir.mkpath(".");

    QUuid presetId = QUuid::createUuid();
    QString presetIdStr = presetId.toString(QUuid::WithoutBraces).left(8);

    auto future = QtConcurrent::run([items = m_items, irDirPath = irDir.absolutePath(), presetIdStr]() {
        ImportResult res;
        res.success = true;
        int firstCoeffCount = 0;

        for (const auto& item : items) {
            try {
                auto coeffs = ConvCoefficientLoader::loadCoefficients(item.filePath.toStdString(),
                                                                      item.format.toStdString(), item.channel);

                if (coeffs.empty()) {
                    res.success = false;
                    res.errorMessage =
                        QString("File '%1' contains zero coefficients.").arg(QFileInfo(item.filePath).fileName());
                    break;
                }

                if (item.invertPhase) {
                    for (double& val : coeffs) {
                        val = -val;
                    }
                }

                if (firstCoeffCount == 0) {
                    firstCoeffCount = static_cast<int>(coeffs.size());
                }

                QString destFileName = QString("Imported-%1-%2.f64").arg(presetIdStr).arg(item.sampleRate);
                QDir dir(irDirPath);
                QString destPath = dir.filePath(destFileName);

                if (!ConvCoefficientLoader::saveRawFloat64(coeffs, destPath.toStdString())) {
                    res.success = false;
                    res.errorMessage = QString("Failed to save imported file: %1").arg(destFileName);
                    break;
                }
                res.paths[item.sampleRate] = destPath.toStdString();
            } catch (const std::exception& ex) {
                res.success = false;
                res.errorMessage = QString::fromStdString(ex.what());
                break;
            } catch (...) {
                res.success = false;
                res.errorMessage = "An unknown error occurred during import.";
                break;
            }
        }
        res.firstCoeffCount = firstCoeffCount;
        return res;
    });

    m_watcher.setFuture(future);
}

void ConvolutionImportDlg::onImportFinished() {
    ImportResult res = m_watcher.result();

    if (res.success) {
        std::string name = m_nameEdit->text().trimmed().toStdString();
        std::string kind = m_kindEdit->text().trimmed().toStdString();
        if (kind.empty())
            kind = "Imported";

        ConvolutionPreset preset(name, res.paths, res.firstCoeffCount, kind);
        m_pipeline->addConvolutionPreset(preset);

        accept();
    } else {
        m_errorLabel->setText(res.errorMessage);
        m_errorWidget->setVisible(true);

        m_isImporting = false;
        m_importBtn->setText("Import");
        m_importBtn->setEnabled(true);
        m_cancelBtn->setEnabled(true);
        m_scrollArea->setEnabled(true);
        updateImportButtonState();
    }
}
