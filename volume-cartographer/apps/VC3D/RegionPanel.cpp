#include "RegionPanel.hpp"

#include "RegionController.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace regions = vc3d::regions;

namespace
{

constexpr int kIdRole = Qt::UserRole + 1;

QIcon swatch(const QString& color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(QColor(30, 30, 30));
    painter.setBrush(QColor(color));
    painter.drawRect(0, 0, 11, 11);
    return QIcon(pixmap);
}

QString groupedCount(std::uint64_t count)
{
    return QLocale().toString(static_cast<qulonglong>(count));
}

QString formatCount(std::uint64_t count)
{
    const double value = static_cast<double>(count);
    if (value >= 1e9) {
        return QStringLiteral("%1 G").arg(value / 1e9, 0, 'f', 2);
    }
    if (value >= 1e6) {
        return QStringLiteral("%1 M").arg(value / 1e6, 0, 'f', 2);
    }
    if (value >= 1e4) {
        return QStringLiteral("%1 k").arg(value / 1e3, 0, 'f', 1);
    }
    return QString::number(static_cast<qulonglong>(count));
}

QString formatBytes(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

// The 256-bin histogram as bars, log scale so sparse bins stay visible.
QPixmap histogramPixmap(const vc3d::regions::Json& counts, const QColor& color)
{
    constexpr int kWidth = 256;
    constexpr int kHeight = 64;
    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(24, 24, 24));
    double top = 0.0;
    for (const auto& c : counts) {
        top = std::max(top, std::log1p(c.get<double>()));
    }
    if (top > 0.0) {
        QPainter painter(&image);
        painter.setPen(color);
        for (int bin = 0; bin < kWidth && bin < static_cast<int>(counts.size()); ++bin) {
            const double h = std::log1p(counts[bin].get<double>()) / top;
            const int height = static_cast<int>(std::lround(h * (kHeight - 2)));
            if (height > 0) {
                painter.drawLine(bin, kHeight - 1, bin, kHeight - 1 - height);
            }
        }
    }
    return QPixmap::fromImage(image.scaled(kWidth, kHeight * 3 / 2));
}

QString joinTags(const std::vector<std::string>& tags)
{
    QStringList list;
    for (const auto& tag : tags) {
        list << QString::fromStdString(tag);
    }
    return list.join(QStringLiteral(", "));
}

}  // namespace

RegionPanel::RegionPanel(RegionController* regions, QWidget* parent) : QDockWidget(tr("Regions"), parent), _regions(regions)
{
    setObjectName(QStringLiteral("regionsDock"));

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    _statusLabel = new QLabel(content);
    _statusLabel->setWordWrap(true);
    _statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(_statusLabel);

    auto* tools = new QHBoxLayout();
    _boxTool = new QToolButton(content);
    _boxTool->setText(tr("3D Box"));
    _boxTool->setCheckable(true);
    _boxTool->setToolTip(
        tr("Drag in the XY, XZ or YZ view to draw a box; its depth is set below. "
           "A click without dragging selects the region under the cursor. Esc leaves the tool."));
    _editTool = new QToolButton(content);
    _editTool->setText(tr("Edit"));
    _editTool->setCheckable(true);
    _editTool->setToolTip(tr("Drag the selected region's handles to move a face or corner, or drag inside it to move it."));
    _depth = new QSpinBox(content);
    _depth->setRange(1, 4096);
    _depth->setSuffix(tr(" vx"));
    _depth->setToolTip(tr("Depth of a new box along the view's normal, in annotation voxels, centered on the slice."));
    tools->addWidget(_boxTool);
    tools->addWidget(_editTool);
    tools->addSpacing(8);
    tools->addWidget(new QLabel(tr("Depth"), content));
    tools->addWidget(_depth);
    tools->addStretch(1);
    layout->addLayout(tools);

    _list = new QTreeWidget(content);
    _list->setColumnCount(3);
    _list->setHeaderLabels({tr("Name"), tr("Size (vx)"), tr("Rev")});
    _list->setRootIsDecorated(false);
    _list->setUniformRowHeights(true);
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->header()->setStretchLastSection(false);
    _list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    _list->setMinimumHeight(110);
    _list->setToolTip(tr("Double-click a region to jump to it."));
    layout->addWidget(_list, 1);

    auto* actions = new QHBoxLayout();
    _jump = new QPushButton(tr("Jump to"), content);
    _jump->setToolTip(tr("Move the focus point to the region's center and center the slice views on it"));
    _focus = new QPushButton(tr("Use as focus box"), content);
    _focus->setToolTip(tr("Dim the voxels outside the region in the XY, XZ and YZ views (the session focus box)"));
    _delete = new QPushButton(tr("Delete"), content);
    _delete->setToolTip(tr("Move the region file to the regions folder's .trash"));
    actions->addWidget(_jump);
    actions->addWidget(_focus);
    actions->addWidget(_delete);
    layout->addLayout(actions);

    _loadErrors = new QLabel(content);
    _loadErrors->setWordWrap(true);
    _loadErrors->setStyleSheet(QStringLiteral("color: #d9534f;"));
    _loadErrors->hide();
    layout->addWidget(_loadErrors);

    _details = new QWidget(content);
    auto* form = new QFormLayout(_details);
    form->setContentsMargins(0, 0, 0, 0);
    auto* nameRow = new QHBoxLayout();
    _name = new QLineEdit(_details);
    _color = new QPushButton(_details);
    _color->setFixedWidth(36);
    _color->setToolTip(tr("Outline color"));
    nameRow->addWidget(_name, 1);
    nameRow->addWidget(_color);
    form->addRow(tr("Name"), nameRow);

    auto* boundsGrid = new QGridLayout();
    const char* axisNames[3] = {"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        boundsGrid->addWidget(new QLabel(QString::fromLatin1(axisNames[axis]), _details), axis, 0);
        for (int side = 0; side < 2; ++side) {
            auto* spin = new QSpinBox(_details);
            spin->setRange(0, std::numeric_limits<int>::max());
            spin->setKeyboardTracking(false);
            spin->setToolTip(side == 0 ? tr("First voxel (inclusive, annotation frame)") : tr("Last voxel (inclusive, annotation frame)"));
            _bounds[axis * 2 + side] = spin;
            boundsGrid->addWidget(spin, axis, side + 1);
        }
    }
    _boundsRow = new QWidget(_details);
    boundsGrid->setContentsMargins(0, 0, 0, 0);
    _boundsRow->setLayout(boundsGrid);
    form->addRow(tr("Bounds"), _boundsRow);

    _info = new QLabel(_details);
    _info->setWordWrap(true);
    _info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString(), _info);
    layout->addWidget(_details);

    _measure = new QGroupBox(tr("Measure and export"), content);
    auto* measure = new QFormLayout(_measure);
    _volume = new QComboBox(_measure);
    _volume->setToolTip(tr("Project volumes this region fits (the current volume first)"));
    measure->addRow(tr("Volume"), _volume);
    _statsLevel = new QComboBox(_measure);
    _statsLevel->setToolTip(
        tr("Pyramid level to read. Auto reads the finest level with at most 32 Mi voxels in the region; "
           "level L voxel i is the sample at full-resolution point i * 2^L."));
    measure->addRow(tr("Level"), _statsLevel);
    auto* thresholdRow = new QHBoxLayout();
    _thresholdOn = new QCheckBox(tr("Count above"), _measure);
    _threshold = new QDoubleSpinBox(_measure);
    _threshold->setRange(0.0, 65535.0);
    _threshold->setDecimals(1);
    _threshold->setValue(128.0);
    _threshold->setEnabled(false);
    thresholdRow->addWidget(_thresholdOn);
    thresholdRow->addWidget(_threshold, 1);
    measure->addRow(tr("Threshold"), thresholdRow);
    auto* statsButtons = new QHBoxLayout();
    _runStats = new QPushButton(tr("Measure"), _measure);
    _runStats->setToolTip(tr("Count, min, max, mean, std, percentiles and a 256-bin histogram of the voxels inside the region"));
    _fullResolution = new QPushButton(tr("Full resolution..."), _measure);
    _fullResolution->setToolTip(tr("Measure at the finest stored level (shows the size first)"));
    _copyJson = new QPushButton(tr("Copy JSON"), _measure);
    _copyJson->setToolTip(tr("Copy the statistics document shown below"));
    statsButtons->addWidget(_runStats);
    statsButtons->addWidget(_fullResolution);
    statsButtons->addWidget(_copyJson);
    measure->addRow(statsButtons);
    _statsSummary = new QLabel(_measure);
    _statsSummary->setWordWrap(true);
    _statsSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    measure->addRow(_statsSummary);
    _histogram = new QLabel(_measure);
    _histogram->setToolTip(tr("Histogram of the region's voxels, 256 bins (uint16: value >> 8), log scale"));
    _histogram->hide();
    measure->addRow(_histogram);

    auto* exportRow = new QHBoxLayout();
    _exportDir = new QLineEdit(_measure);
    _exportDir->setPlaceholderText(tr("default: regions/<project>/exports/..."));
    _exportDir->setToolTip(tr("An absent or empty folder for the crop (volume.zarr and manifest.json)"));
    _browseExport = new QToolButton(_measure);
    _browseExport->setText(QStringLiteral("..."));
    exportRow->addWidget(_exportDir, 1);
    exportRow->addWidget(_browseExport);
    measure->addRow(tr("Export to"), exportRow);
    auto* exportLevelRow = new QHBoxLayout();
    _exportLevel = new QComboBox(_measure);
    _runExport = new QPushButton(tr("Export crop"), _measure);
    _runExport->setToolTip(tr("Save the region's voxels at this level as a local zarr volume with a manifest that places it in the scan"));
    exportLevelRow->addWidget(_exportLevel, 1);
    exportLevelRow->addWidget(_runExport);
    measure->addRow(tr("Export level"), exportLevelRow);

    auto* jobRow = new QHBoxLayout();
    _jobProgress = new QProgressBar(_measure);
    _jobProgress->setRange(0, 1000);
    _jobProgress->setTextVisible(false);
    _cancelJob = new QPushButton(tr("Cancel"), _measure);
    jobRow->addWidget(_jobProgress, 1);
    jobRow->addWidget(_cancelJob);
    measure->addRow(jobRow);
    _jobStatus = new QLabel(_measure);
    _jobStatus->setWordWrap(true);
    _jobStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    measure->addRow(_jobStatus);
    layout->addWidget(_measure);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    setWidget(scroll);

    if (!_regions) {
        return;
    }

    connect(_boxTool, &QToolButton::toggled, this, [this](bool on) {
        if (_populating) {
            return;
        }
        _regions->setTool(on ? RegionController::Tool::Box : RegionController::Tool::None);
    });
    connect(_editTool, &QToolButton::toggled, this, [this](bool on) {
        if (_populating) {
            return;
        }
        _regions->setEditing(on);
    });
    connect(_depth, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (!_populating) {
            _regions->setNewBoxDepth(value);
        }
    });
    connect(_list, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (_populating) {
            return;
        }
        _regions->setActive(selectedId());
    });
    connect(_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) {
            return;
        }
        const auto result = _regions->jumpTo(item->data(0, kIdRole).toString());
        if (!result.ok()) {
            showError(result.message);
        }
    });
    connect(_name, &QLineEdit::editingFinished, this, &RegionPanel::applyName);
    connect(_color, &QPushButton::clicked, this, &RegionPanel::pickColor);
    for (auto* spin : _bounds) {
        connect(spin, &QSpinBox::editingFinished, this, &RegionPanel::applyBounds);
    }
    connect(_jump, &QPushButton::clicked, this, [this]() {
        const auto result = _regions->jumpTo(selectedId());
        if (!result.ok()) {
            showError(result.message);
        }
    });
    connect(_focus, &QPushButton::clicked, this, [this]() {
        const auto result = _regions->useAsFocusBox(selectedId());
        if (!result.ok()) {
            showError(result.message);
        }
    });
    connect(_delete, &QPushButton::clicked, this, [this]() {
        const QString id = selectedId();
        const auto* loaded = _regions->find(id);
        if (!loaded) {
            return;
        }
        const auto result = _regions->remove(id, loaded->region.revision);
        if (!result.ok()) {
            showError(result.message);
        }
    });

    connect(_regions, &RegionController::regionsChanged, this, [this]() {
        refreshList();
        refreshDetails();
        refreshTools();
        refreshMeasure();
    });
    connect(_regions, &RegionController::activeRegionChanged, this, [this](const QString&) {
        showError(QString());
        selectActiveInList();
        refreshDetails();
        refreshTools();
        refreshMeasure();
    });
    connect(_regions, &RegionController::toolChanged, this, &RegionPanel::refreshTools);

    connect(_volume, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!_populating) {
            refreshLevels();
        }
    });
    connect(_thresholdOn, &QCheckBox::toggled, _threshold, &QWidget::setEnabled);
    connect(_runStats, &QPushButton::clicked, this, [this]() { runStats(false); });
    connect(_fullResolution, &QPushButton::clicked, this, [this]() { runStats(true); });
    connect(_copyJson, &QPushButton::clicked, this, [this]() {
        if (!_statsJson.isEmpty()) {
            QApplication::clipboard()->setText(_statsJson);
        }
    });
    connect(_browseExport, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for the crop"), _exportDir->text());
        if (!dir.isEmpty()) {
            _exportDir->setText(dir);
        }
    });
    connect(_runExport, &QPushButton::clicked, this, &RegionPanel::runExport);
    connect(_cancelJob, &QPushButton::clicked, this, [this]() { _regions->cancelJob(); });
    connect(_regions, &RegionController::jobStarted, this, [this](const QString&, const QString& label, const QStringList&) {
        _jobProgress->setValue(0);
        _jobStatus->setStyleSheet(QString());
        _jobStatus->setText(label + QStringLiteral("..."));
        refreshJobControls();
    });
    connect(_regions, &RegionController::jobProgress, this, [this](const QString&, double fraction) {
        _jobProgress->setValue(static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 1000.0)));
    });
    connect(_regions, &RegionController::jobFinished, this, [this](const QString& kind, bool success, const QString& message, const QJsonObject&, const QStringList&) {
        _jobProgress->setValue(0);
        _jobStatus->setStyleSheet(success ? QString() : QStringLiteral("color: #d9534f;"));
        _jobStatus->setText(message);
        refreshJobControls();
        if (kind == QLatin1String(RegionController::kStatsJobKind)) {
            refreshSavedStats();
        }
    });
    connect(_regions, &RegionController::savedStatsChanged, this, [this](const QString& id) {
        if (id == _regions->activeId()) {
            refreshSavedStats();
        }
    });

    refreshAll();
}

void RegionPanel::showEvent(QShowEvent* event)
{
    QDockWidget::showEvent(event);
    if (_regions && !_regions->syncDirectory()) {
        refreshAll();
    }
}

void RegionPanel::refreshAll()
{
    refreshTools();
    refreshList();
    refreshDetails();
    refreshMeasure();
}

void RegionPanel::refreshMeasure()
{
    if (!_regions) {
        return;
    }
    const QString id = _regions->activeId();
    const QString previous = _volume->currentData().toString();
    _populating = true;
    _volume->clear();
    for (const auto& choice : _regions->compatibleVolumes(id)) {
        _volume->addItem(choice.current ? tr("%1 (current)").arg(choice.id) : choice.id, choice.id);
    }
    const int keep = _volume->findData(previous);
    if (keep >= 0) {
        _volume->setCurrentIndex(keep);
    }
    _populating = false;
    refreshLevels();
    refreshSavedStats();
}

void RegionPanel::refreshLevels()
{
    if (!_regions) {
        return;
    }
    const QString id = _regions->activeId();
    const QString volumeId = _volume->currentData().toString();
    const auto estimates = _regions->levelEstimates(id, volumeId);
    const int autoLevel = RegionController::autoLevel(estimates);
    const int previousStats = _statsLevel->currentData().isValid() ? _statsLevel->currentData().toInt() : -1;
    const int previousExport = _exportLevel->currentData().isValid() ? _exportLevel->currentData().toInt() : autoLevel;
    _populating = true;
    _statsLevel->clear();
    _exportLevel->clear();
    if (!estimates.empty()) {
        _statsLevel->addItem(tr("Auto (level %1)").arg(autoLevel), -1);
    }
    for (const auto& estimate : estimates) {
        const QString text = tr("Level %1: %2 voxels, %3").arg(estimate.level).arg(formatCount(estimate.voxels), formatBytes(estimate.bytes));
        _statsLevel->addItem(text, estimate.level);
        _exportLevel->addItem(text, estimate.level);
    }
    if (const int i = _statsLevel->findData(previousStats); i >= 0) {
        _statsLevel->setCurrentIndex(i);
    }
    if (const int i = _exportLevel->findData(previousExport); i >= 0) {
        _exportLevel->setCurrentIndex(i);
    } else if (const int j = _exportLevel->findData(autoLevel); j >= 0) {
        _exportLevel->setCurrentIndex(j);
    }
    _populating = false;
    refreshJobControls();
}

void RegionPanel::refreshJobControls()
{
    if (!_regions) {
        return;
    }
    const regions::Loaded* loaded = _regions->find(_regions->activeId());
    const bool measurable = loaded && !loaded->readOnly && _volume->count() > 0 && _statsLevel->count() > 0;
    const bool running = _regions->jobRunning();
    _measure->setEnabled(loaded != nullptr || running);
    _runStats->setEnabled(measurable && !running);
    _fullResolution->setEnabled(measurable && !running);
    _runExport->setEnabled(measurable && !running && _exportLevel->count() > 0);
    _cancelJob->setEnabled(running);
    _jobProgress->setVisible(running);
    _copyJson->setEnabled(!_statsJson.isEmpty());
}

void RegionPanel::refreshSavedStats()
{
    if (!_regions) {
        return;
    }
    const auto saved = _regions->savedStats(_regions->activeId());
    if (saved.empty()) {
        _statsJson.clear();
        _statsSummary->setText(_regions->find(_regions->activeId()) ? tr("Not measured yet.") : QString());
        _histogram->hide();
        refreshJobControls();
        return;
    }
    showStats(saved.front().document, saved.front().stale);
}

void RegionPanel::showStats(const regions::Json& doc, bool stale)
{
    _statsJson = QString::fromStdString(doc.dump(2));
    const auto number = [&doc](const char* key) { return doc.contains(key) && doc[key].is_number() ? doc[key].get<double>() : 0.0; };
    QStringList lines;
    const std::string volumeId = doc.contains("volume") && doc["volume"].is_object() ? doc["volume"].value("id", std::string()) : std::string();
    lines << tr("<b>%1</b>, level %2%3")
                 .arg(QString::fromStdString(volumeId).toHtmlEscaped())
                 .arg(doc.value("level", 0))
                 .arg(stale ? tr(" <span style='color:#d9534f'>(stale: the region changed since)</span>") : QString());
    lines << tr("%1 voxels (%2 mm<sup>3</sup>), min %3, max %4")
                 .arg(groupedCount(doc.value("count", std::uint64_t{0})))
                 .arg(number("volume_mm3"), 0, 'g', 4)
                 .arg(doc.value("min", 0))
                 .arg(doc.value("max", 0));
    lines << tr("mean %1, std %2").arg(number("mean"), 0, 'f', 3).arg(number("std"), 0, 'f', 3);
    if (doc.contains("percentiles") && doc["percentiles"].is_array()) {
        QStringList parts;
        for (const auto& entry : doc["percentiles"]) {
            parts << QStringLiteral("p%1 %2").arg(entry.value("p", 0.0)).arg(entry.value("value", 0));
        }
        lines << parts.join(QStringLiteral(", "));
    }
    if (doc.contains("threshold") && doc["threshold"].is_number()) {
        lines << tr("above %1: %2 (%3%)")
                     .arg(number("threshold"))
                     .arg(groupedCount(doc.value("count_above", std::uint64_t{0})))
                     .arg(number("fraction_above") * 100.0, 0, 'f', 3);
    }
    lines << tr("%1 chunks read (%2 fill), %3 s").arg(doc.value("chunks_read", 0)).arg(doc.value("fill_chunks", 0)).arg(number("seconds"), 0, 'f', 2);
    _statsSummary->setText(lines.join(QStringLiteral("<br>")));
    if (doc.contains("histogram") && doc["histogram"].contains("counts") && doc["histogram"]["counts"].is_array()) {
        QColor color(255, 176, 0);
        if (const regions::Loaded* loaded = _regions->find(_regions->activeId())) {
            color = QColor(QString::fromStdString(loaded->region.color));
        }
        _histogram->setPixmap(histogramPixmap(doc["histogram"]["counts"], color));
        _histogram->show();
    } else {
        _histogram->hide();
    }
    refreshJobControls();
}

void RegionPanel::runStats(bool fullResolution)
{
    if (!_regions) {
        return;
    }
    const QString id = _regions->activeId();
    const QString volumeId = _volume->currentData().toString();
    RegionController::StatsJob job;
    job.ids = {id};
    job.volumeId = volumeId;
    job.level = _statsLevel->currentData().isValid() ? _statsLevel->currentData().toInt() : -1;
    if (fullResolution) {
        const auto estimates = _regions->levelEstimates(id, volumeId);
        if (estimates.empty()) {
            return;
        }
        const auto& finest = estimates.front();
        const auto answer = QMessageBox::question(
            this,
            tr("Measure at full resolution"),
            tr("Level %1 has %2 voxels (%3) in this region. Reading them may take a while and evicts cached chunks. Measure?")
                .arg(finest.level)
                .arg(formatCount(finest.voxels), formatBytes(finest.bytes)));
        if (answer != QMessageBox::Yes) {
            return;
        }
        job.level = finest.level;
    }
    if (_thresholdOn->isChecked()) {
        job.threshold = _threshold->value();
    }
    const auto result = _regions->startStats(job);
    if (!result.ok()) {
        _jobStatus->setStyleSheet(QStringLiteral("color: #d9534f;"));
        _jobStatus->setText(result.message);
    }
}

void RegionPanel::runExport()
{
    if (!_regions || !_exportLevel->currentData().isValid()) {
        return;
    }
    const auto result =
        _regions
            ->startExport(_regions->activeId(), _volume->currentData().toString(), _exportLevel->currentData().toInt(), _exportDir->text().trimmed());
    if (!result.ok()) {
        _jobStatus->setStyleSheet(QStringLiteral("color: #d9534f;"));
        _jobStatus->setText(result.message);
    }
}

void RegionPanel::refreshTools()
{
    if (!_regions) {
        return;
    }
    const QString reason = _regions->unavailableReason();
    _populating = true;
    _boxTool->setEnabled(reason.isEmpty());
    _editTool->setEnabled(reason.isEmpty());
    _boxTool->setChecked(_regions->tool() == RegionController::Tool::Box);
    _editTool->setChecked(_regions->editing());
    _depth->setValue(_regions->newBoxDepth());
    _populating = false;
    if (!reason.isEmpty()) {
        _statusLabel->setText(reason);
        _statusLabel->setStyleSheet(QString());
        _statusLabel->show();
    } else if (_statusLabel->property("vc_error").toBool()) {
        _statusLabel->show();
    } else {
        _statusLabel->setText(QString());
        _statusLabel->hide();
    }
    const auto& errors = _regions->loadErrors();
    if (errors.empty()) {
        _loadErrors->hide();
    } else {
        QStringList lines;
        for (const auto& error : errors) {
            QString line = QString::fromStdString(error.file) + QStringLiteral(": ");
            if (!error.path.empty()) {
                line += QString::fromStdString(error.path) + QStringLiteral(": ");
            }
            line += QString::fromStdString(error.message);
            lines << line;
        }
        _loadErrors->setText(tr("Could not read %n region file(s):\n", nullptr, static_cast<int>(errors.size())) + lines.join('\n'));
        _loadErrors->show();
    }
}

void RegionPanel::refreshList()
{
    if (!_regions) {
        return;
    }
    _populating = true;
    _list->clear();
    for (const auto& loaded : _regions->regions()) {
        const auto& region = loaded.region;
        auto* item = new QTreeWidgetItem(_list);
        item->setData(0, kIdRole, QString::fromStdString(region.id));
        item->setText(0, QString::fromStdString(region.name));
        item->setIcon(0, swatch(QString::fromStdString(region.color)));
        if (const regions::Box* box = regions::boxOf(region)) {
            item->setText(1, QStringLiteral("%1 x %2 x %3").arg(box->max[0] - box->min[0] + 1).arg(box->max[1] - box->min[1] + 1).arg(box->max[2] - box->min[2] + 1));
        }
        item->setText(2, QString::number(region.revision));
        QString tooltip = QString::fromStdString(regions::describeBox(region.shape));
        const regions::FrameCheck check = _regions->frameCheck(region);
        if (!check.map || loaded.readOnly) {
            for (int column = 0; column < 3; ++column) {
                item->setForeground(column, QColor(140, 140, 140));
            }
            tooltip = !check.map ? QString::fromStdString(check.reason) : tr("Read-only: %1").arg(QString::fromStdString(loaded.readOnlyReason));
        }
        for (int column = 0; column < 3; ++column) {
            item->setToolTip(column, tooltip);
        }
    }
    _populating = false;
    selectActiveInList();
}

void RegionPanel::selectActiveInList()
{
    if (!_regions) {
        return;
    }
    const QString active = _regions->activeId();
    _populating = true;
    _list->clearSelection();
    for (int i = 0; i < _list->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = _list->topLevelItem(i);
        if (item->data(0, kIdRole).toString() == active) {
            item->setSelected(true);
            _list->scrollToItem(item);
            break;
        }
    }
    _populating = false;
}

QString RegionPanel::selectedId() const
{
    const auto items = _list->selectedItems();
    return items.isEmpty() ? QString() : items.front()->data(0, kIdRole).toString();
}

void RegionPanel::refreshDetails()
{
    if (!_regions) {
        return;
    }
    const regions::Loaded* loaded = _regions->find(_regions->activeId());
    const bool have = loaded != nullptr;
    const regions::FrameCheck check = have ? _regions->frameCheck(loaded->region) : regions::FrameCheck{};
    const bool editable = have && !loaded->readOnly && check.map.has_value();
    _details->setEnabled(have);
    for (QWidget* w : std::initializer_list<QWidget*>{_name, _color}) {
        w->setEnabled(editable);
    }
    for (auto* spin : _bounds) {
        spin->setEnabled(editable);
    }
    _jump->setEnabled(have && check.map.has_value());
    _focus->setEnabled(have && check.map.has_value());
    _delete->setEnabled(have);

    _populating = true;
    const QSignalBlocker b1(_name);
    if (!have) {
        _name->clear();
        _color->setIcon(QIcon());
        for (auto* spin : _bounds) {
            const QSignalBlocker block(spin);
            spin->setValue(0);
        }
        _info->setText(tr("Select a region, or draw one with 3D Box."));
        _populating = false;
        return;
    }
    const auto& region = loaded->region;
    _name->setText(QString::fromStdString(region.name));
    _color->setIcon(swatch(QString::fromStdString(region.color)));
    const cv::Vec3i maxIndex = regions::frameMaxIndexXyz(region.frame);
    QStringList info;
    if (const regions::Box* box = regions::boxOf(region)) {
        for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
                QSpinBox* spin = _bounds[axis * 2 + side];
                const QSignalBlocker block(spin);
                spin->setRange(0, std::max(0, maxIndex[axis]));
                spin->setValue(side == 0 ? box->min[axis] : box->max[axis]);
            }
        }
        info << tr("%1 voxels in the annotation frame").arg(groupedCount(regions::voxelCount(*box)));
    }
    if (!region.tags.empty()) {
        info << tr("Tags: %1").arg(joinTags(region.tags));
    }
    if (!region.notes.empty()) {
        info << tr("Notes: %1").arg(QString::fromStdString(region.notes));
    }
    if (!region.author.empty()) {
        info << tr("By %1").arg(QString::fromStdString(region.author));
    }
    info << tr("Created %1, modified %2, revision %3")
                .arg(QString::fromStdString(region.created), QString::fromStdString(region.modified))
                .arg(region.revision);
    if (loaded->readOnly) {
        info << tr("Read-only: %1").arg(QString::fromStdString(loaded->readOnlyReason));
    }
    if (!check.map) {
        info << QString::fromStdString(check.reason);
    }
    _info->setText(info.join('\n'));
    _populating = false;
}

void RegionPanel::showError(const QString& message)
{
    _statusLabel->setProperty("vc_error", !message.isEmpty());
    if (message.isEmpty()) {
        refreshTools();
        return;
    }
    _statusLabel->setText(message);
    _statusLabel->setStyleSheet(QStringLiteral("color: #d9534f;"));
    _statusLabel->setVisible(true);
}

void RegionPanel::applyName()
{
    if (_populating || !_regions) {
        return;
    }
    const regions::Loaded* loaded = _regions->find(_regions->activeId());
    if (!loaded || loaded->region.name == _name->text().trimmed().toStdString()) {
        return;
    }
    regions::Region next = loaded->region;
    next.name = _name->text().trimmed().toStdString();
    const auto result = _regions->update(std::move(next), loaded->region.revision);
    if (!result.ok()) {
        showError(result.message);
        refreshDetails();
    }
}

void RegionPanel::applyBounds()
{
    if (_populating || !_regions) {
        return;
    }
    const regions::Loaded* loaded = _regions->find(_regions->activeId());
    if (!loaded) {
        return;
    }
    regions::Box box;
    for (int axis = 0; axis < 3; ++axis) {
        const int a = _bounds[axis * 2]->value();
        const int b = _bounds[axis * 2 + 1]->value();
        box.min[axis] = std::min(a, b);
        box.max[axis] = std::max(a, b);
    }
    if (!regions::boxOf(loaded->region) || box == *regions::boxOf(loaded->region)) {
        return;
    }
    regions::Region next = loaded->region;
    next.shape = box;
    const auto result = _regions->update(std::move(next), loaded->region.revision);
    if (!result.ok()) {
        showError(result.message);
        refreshDetails();
    }
}

void RegionPanel::pickColor()
{
    if (!_regions) {
        return;
    }
    const regions::Loaded* loaded = _regions->find(_regions->activeId());
    if (!loaded) {
        return;
    }
    const QColor chosen = QColorDialog::getColor(QColor(QString::fromStdString(loaded->region.color)), this, tr("Region color"));
    if (!chosen.isValid()) {
        return;
    }
    const std::string color = chosen.name(QColor::HexRgb).toStdString();
    if (color == loaded->region.color) {
        return;
    }
    const std::uint64_t revision = loaded->region.revision;
    regions::Region next = loaded->region;
    next.color = color;
    const auto result = _regions->update(std::move(next), revision);
    if (!result.ok()) {
        showError(result.message);
    }
}
