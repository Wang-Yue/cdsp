#include "ui/OratoryPresetPickerDlg.h"

#include "ui/StyleTheme.h"

#include <QHBoxLayout>
#include <QMessageBox>
#include <QRegularExpression>
#include <QVBoxLayout>

OratoryPresetPickerDlg::OratoryPresetPickerDlg(std::shared_ptr<PipelineStore> pipeline,
                                               std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline), m_dspController(dspController) {
    setWindowTitle("Oratory1990 Database");
    resize(500, 600);
    setupUi();
    loadIndex(false);
}

void OratoryPresetPickerDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    auto headerTitle = new QLabel("Oratory1990 Database", this);
    headerTitle->setFont(QFont("sans-serif", 13, QFont::Bold));
    mainLayout->addWidget(headerTitle);

    auto subtitleLbl = new QLabel("Hand-measured presets based on Oratory1990 targets", this);
    subtitleLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    mainLayout->addWidget(subtitleLbl);

    auto searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search 0 headphones...");
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_listWidget->currentItem() == nullptr && m_listWidget->count() > 0) {
            m_listWidget->setCurrentRow(0);
        }
        onImportClicked();
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, &OratoryPresetPickerDlg::onSearchTextChanged);
    searchLayout->addWidget(m_searchEdit);

    auto refreshBtn = new QPushButton("🔄 Refresh Database", this);
    connect(refreshBtn, &QPushButton::clicked, [this]() {
        m_statusLabel->setText("Refreshing database from GitHub...");
        loadIndex(true);
    });
    searchLayout->addWidget(refreshBtn);

    mainLayout->addLayout(searchLayout);

    m_listWidget = new QListWidget(this);
    m_listWidget->setAlternatingRowColors(true);
    connect(m_listWidget, &QListWidget::itemActivated, this, &OratoryPresetPickerDlg::onImportClicked);
    mainLayout->addWidget(m_listWidget);

    m_statusLabel = new QLabel("Loading Oratory1990 index...", this);
    m_statusLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    mainLayout->addWidget(m_statusLabel);

    auto btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    m_importBtn = new QPushButton("Import Selected Preset", this);
    m_importBtn->setStyleSheet(
        "background-color: #007af5; color: white; font-weight: bold; padding: 5px 14px; border-radius: 4px;");
    m_importBtn->setDefault(true);
    m_importBtn->setAutoDefault(true);
    m_importBtn->setEnabled(false);
    connect(m_importBtn, &QPushButton::clicked, this, &OratoryPresetPickerDlg::onImportClicked);
    btnLayout->addWidget(m_importBtn);

    auto closeBtn = new QPushButton("Cancel", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(closeBtn);

    mainLayout->addLayout(btnLayout);

    connect(m_listWidget, &QListWidget::itemSelectionChanged,
            [this]() { m_importBtn->setEnabled(!m_listWidget->selectedItems().isEmpty()); });
}

void OratoryPresetPickerDlg::loadIndex(bool forceRefresh) {
    QPointer<OratoryPresetPickerDlg> safeThis(this);
    m_service.fetchIndex(
        [safeThis](bool ok, const std::vector<OratoryIndexEntry>& entries) {
            if (!safeThis)
                return;
            if (ok) {
                safeThis->m_entries = entries;
                safeThis->m_statusLabel->setText(QString("Loaded %1 Oratory presets.").arg(entries.size()));
                safeThis->m_searchEdit->setPlaceholderText(QString("Search %1 headphones...").arg(entries.size()));
                safeThis->onSearchTextChanged(safeThis->m_searchEdit->text());
            } else {
                safeThis->m_statusLabel->setText("Failed to load Oratory1990 index.");
            }
        },
        forceRefresh);
}

void OratoryPresetPickerDlg::onSearchTextChanged(const QString& text) {
    m_listWidget->clear();
    QString trimmed = text.trimmed();
    QStringList tokens = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    int count = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (tokens.isEmpty() && count >= 50)
            break;

        QString name = QString::fromStdString(m_entries[i].name);
        QString path = QString::fromStdString(m_entries[i].path);

        bool matches = true;
        for (const auto& token : tokens) {
            if (!name.contains(token, Qt::CaseInsensitive) && !path.contains(token, Qt::CaseInsensitive)) {
                matches = false;
                break;
            }
        }

        if (matches) {
            auto item = new QListWidgetItem(m_listWidget);
            item->setText(QString("%1\n%2").arg(name, path));
            item->setData(Qt::UserRole, static_cast<int>(i));
            count++;
        }
    }

    if (tokens.isEmpty()) {
        m_statusLabel->setText(
            QString("Showing %1 headphones (out of %2 total).").arg(m_listWidget->count()).arg(m_entries.size()));
    } else {
        m_statusLabel->setText(QString("Found %1 of %2 matching headphones.").arg(count).arg(m_entries.size()));
    }
}

void OratoryPresetPickerDlg::onImportClicked() {
    auto items = m_listWidget->selectedItems();
    if (items.isEmpty())
        return;

    int idx = items[0]->data(Qt::UserRole).toInt();
    const auto& entry = m_entries[idx];

    m_statusLabel->setText("Downloading preset...");
    m_importBtn->setEnabled(false);

    m_service.fetchPreset(entry, [this, entry](bool ok, std::optional<EQPreset> preset) {
        m_importBtn->setEnabled(true);
        if (ok && preset.has_value()) {
            auto p = preset.value();
            p.name = "[Oratory] " + entry.name;

            // Update existing or add new EQPreset
            QUuid presetId;
            bool foundExisting = false;
            for (auto& existing : m_pipeline->eqPresets) {
                if (existing.name == p.name) {
                    p.id = existing.id;
                    m_pipeline->updateEQPreset(p);
                    presetId = p.id;
                    foundExisting = true;
                    break;
                }
            }
            if (!foundExisting) {
                presetId = m_pipeline->addEQPreset(p);
            }

            // Direct Stage Creation or Overwrite in PipelineStore
            bool stageUpdated = false;
            for (auto& stage : m_pipeline->stages) {
                if (stage.type == StageType::EQ) {
                    stage.eqPresetId = presetId;
                    stage.name = p.name;
                    stageUpdated = true;
                    break;
                }
            }
            if (!stageUpdated) {
                PipelineStage newStage(StageType::EQ, p.name);
                newStage.eqPresetId = presetId;
                m_pipeline->stages.push_back(newStage);
            }

            m_pipeline->save();
            emit m_pipeline->pipelineChanged();

            // Direct call to DSPEngineController::applyConfig()
            if (m_dspController) {
                m_dspController->applyConfig();
            }

            QMessageBox::information(
                this, "Success",
                QString("Imported preset '%1' and active EQ stage updated.").arg(QString::fromStdString(p.name)));
            accept();
        } else {
            m_statusLabel->setText("Failed to download preset.");
        }
    });
}
