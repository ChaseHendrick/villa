#include "RegionPanel.hpp"

#include "RegionController.hpp"

#include <QColorDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
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
    });
    connect(_regions, &RegionController::activeRegionChanged, this, [this](const QString&) {
        showError(QString());
        selectActiveInList();
        refreshDetails();
        refreshTools();
    });
    connect(_regions, &RegionController::toolChanged, this, &RegionPanel::refreshTools);

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
