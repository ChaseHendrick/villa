#include "RegionController.hpp"

#include "CState.hpp"
#include "OpenDataCoordinateIdentity.hpp"
#include "ProjectFiberPaths.hpp"
#include "VCSettings.hpp"
#include "ViewerManager.hpp"
#include "volume_viewers/VolumeViewerBase.hpp"

#include "vc/core/types/VolumePkg.hpp"
#include "vc/core/util/Logging.hpp"
#include "vc/core/util/PlaneSurface.hpp"

#include <QPolygonF>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>

namespace regions = vc3d::regions;

namespace
{

constexpr auto kNewBoxDepthKey = "regions/new_box_depth";
constexpr int kDefaultNewBoxDepth = 64;
constexpr int kMaxNewBoxDepth = 4096;
// A press that moves less than this many scene pixels is a click.
constexpr double kClickSlopPx = 3.0;
// Handles are picked within this many scene pixels.
constexpr double kHandlePickPx = 8.0;

const char* const kPalette[] = {"#ffb000", "#00c2ff", "#ff4fa3", "#7cff4f", "#b388ff", "#ff6e40", "#40e0d0", "#ffd740"};

double sceneDistance(const QPointF& a, const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QString boxText(const regions::Box& box)
{
    return QString::fromStdString(regions::describeBox(box));
}

}  // namespace

RegionController::RegionController(CState* state, ViewerManager* viewers, QObject* parent)
    : QObject(parent), _state(state), _viewers(viewers)
{
    {
        QSettings settings(vc3d::settingsFilePath(), QSettings::IniFormat);
        _newBoxDepth = std::clamp(settings.value(kNewBoxDepthKey, kDefaultNewBoxDepth).toInt(), 1, kMaxNewBoxDepth);
    }
    if (_state) {
        connect(_state, &CState::vpkgChanged, this, [this](const std::shared_ptr<VolumePkg>&) { onProjectChanged(); });
        connect(_state, &CState::volumeChanged, this, [this](const std::shared_ptr<Volume>&, const std::string&) { onVolumeChanged(); });
        connect(_state, &CState::volumeClosing, this, [this]() { cancelGestures(); });
    }
    if (_viewers) {
        _viewers->forEachBaseViewer([this](VolumeViewerBase* viewer) { attachViewer(viewer); });
        connect(_viewers, &ViewerManager::baseViewerCreated, this, [this](VolumeViewerBase* viewer) { attachViewer(viewer); });
    }
    onProjectChanged();
}

RegionController::~RegionController()
{
    for (const auto& viewer : _toolViewers) {
        if (viewer) {
            viewer->setPointerOverride({});
        }
    }
}

void RegionController::attachViewer(VolumeViewerBase* base)
{
    auto* viewer = base ? qobject_cast<CChunkedVolumeViewer*>(base->asQObject()) : nullptr;
    if (!viewer) {
        return;
    }
    const QString role = viewer->property("vc_viewer_role").toString();
    if (!regions::shouldInstallTools(role.toStdString(), viewer->surfName())) {
        return;
    }
    if (viewer->hasPointerOverride()) {
        Logger()->warn("Regions: viewer '{}' already has a pointer override; region tools are not available there", viewer->surfName());
        return;
    }
    QPointer<RegionController> self(this);
    QPointer<CChunkedVolumeViewer> guard(viewer);
    viewer->setPointerOverride([self, guard](const CChunkedVolumeViewer::PointerEvent& event) {
        if (!self || !guard) {
            return false;
        }
        return self->handlePointer(guard.data(), event);
    });
    _toolViewers.erase(std::remove_if(_toolViewers.begin(), _toolViewers.end(), [](const auto& v) { return v.isNull(); }), _toolViewers.end());
    _toolViewers.push_back(guard);
}

bool RegionController::syncDirectory()
{
    const std::filesystem::path path = directory().toStdU16String();
    if (path == _store.directory()) {
        return false;
    }
    cancelDrag(false);
    _store.setDirectory(path);
    _activeId.clear();
    rebuildFrameChecks();
    emit regionsChanged();
    emit activeRegionChanged(_activeId);
    return true;
}

QString RegionController::directory() const
{
    const auto pkg = _state ? _state->vpkg() : nullptr;
    if (!pkg) {
        return {};
    }
    return vc3d::pathToQString(vc3d::projectAnnotationDirectory(pkg->path(), "regions", pkg->getVolpkgDirectory()));
}

const regions::Loaded* RegionController::find(const QString& id) const
{
    return _store.find(id.toStdString());
}

regions::FrameCheck RegionController::frameCheck(const regions::Region& region) const
{
    const auto it = _frameChecks.find(region.id);
    if (it != _frameChecks.end()) {
        return it->second;
    }
    return regions::mapRegionFrame(region.frame, _volumeFrame, _volumeSpace);
}

QString RegionController::unavailableReason() const
{
    if (!_state || !_state->vpkg() || directory().isEmpty()) {
        return tr("Save the project first; regions are stored next to the project file.");
    }
    if (!_state->currentVolume()) {
        return tr("Open a volume first.");
    }
    if (!currentFrame()) {
        return tr("This volume's annotation frame is unknown, so regions cannot be placed on it.");
    }
    return {};
}

QString RegionController::currentVolumeId() const
{
    return _state ? QString::fromStdString(_state->currentVolumeId()) : QString();
}

std::optional<regions::Frame> RegionController::currentFrame() const
{
    if (!_state || !_state->currentVolume()) {
        return std::nullopt;
    }
    regions::Frame frame = regions::frameForVolume(_volumeFrame, _state->currentVolumeId(), _volumeIdentity);
    if (frame.baseShapeZyx[0] <= 0 || frame.baseShapeZyx[1] <= 0 || frame.baseShapeZyx[2] <= 0) {
        return std::nullopt;
    }
    return frame;
}

std::optional<regions::FrameMap> RegionController::currentMap() const
{
    const auto frame = currentFrame();
    if (!frame) {
        return std::nullopt;
    }
    return regions::mapRegionFrame(*frame, _volumeFrame, _volumeSpace).map;
}

void RegionController::refreshVolumeFrame()
{
    _volumeFrame = {};
    _volumeSpace.clear();
    _volumeIdentity = regions::Json::object();
    if (!_state || !_state->currentVolume()) {
        return;
    }
    const auto pkg = _state->vpkg();
    const std::string volumeId = _state->currentVolumeId();
    _volumeFrame = vc3d::opendata::annotationFrameForVolume(pkg.get(), volumeId, *_state->currentVolume());
    if (pkg && !volumeId.empty()) {
        try {
            _volumeSpace = vc3d::opendata::coordinateSpaceTag(pkg->volumeTags(volumeId));
            const auto identity = vc3d::opendata::coordinateIdentityJson(vc3d::opendata::coordinateIdentityForVolume(*pkg, volumeId));
            _volumeIdentity = regions::Json::parse(identity.dump());
        } catch (const std::exception& e) {
            Logger()->warn("Regions: could not read the coordinate identity of volume '{}': {}", volumeId, e.what());
            _volumeIdentity = regions::Json::object();
        }
        if (!_volumeSpace.empty() && !_volumeIdentity.contains("vc_open_data_coordinate_space")) {
            _volumeIdentity["vc_open_data_coordinate_space"] = _volumeSpace;
        }
    }
}

void RegionController::rebuildFrameChecks()
{
    _frameChecks.clear();
    for (const auto& loaded : _store.regions()) {
        _frameChecks[loaded.region.id] = regions::mapRegionFrame(loaded.region.frame, _volumeFrame, _volumeSpace);
    }
}

void RegionController::onProjectChanged()
{
    cancelDrag(false);
    refreshVolumeFrame();
    _store.setDirectory(directory().toStdU16String());
    _activeId.clear();
    rebuildFrameChecks();
    emit regionsChanged();
    emit activeRegionChanged(_activeId);
}

void RegionController::onVolumeChanged()
{
    cancelDrag(false);
    refreshVolumeFrame();
    syncDirectory();
    rebuildFrameChecks();
    emit regionsChanged();
}

void RegionController::emitChanged(const Result& result)
{
    rebuildFrameChecks();
    if (!_activeId.isEmpty() && !find(_activeId)) {
        _activeId.clear();
        emit activeRegionChanged(_activeId);
    }
    emit regionsChanged();
    if (!result.ok()) {
        emit statusMessage(result.message, 6000);
    }
}

QString RegionController::nextName() const
{
    int highest = 0;
    for (const auto& loaded : _store.regions()) {
        const QString name = QString::fromStdString(loaded.region.name);
        if (name.startsWith(QStringLiteral("Region "))) {
            bool ok = false;
            const int n = name.mid(7).toInt(&ok);
            if (ok) {
                highest = std::max(highest, n);
            }
        }
    }
    return QStringLiteral("Region %1").arg(highest + 1);
}

QString RegionController::nextColor(std::size_t index)
{
    return QString::fromLatin1(kPalette[index % (sizeof(kPalette) / sizeof(kPalette[0]))]);
}

RegionController::Result RegionController::create(regions::Region draft)
{
    syncDirectory();
    Result result;
    if (directory().isEmpty()) {
        result.code = Code::NoProject;
        result.message = unavailableReason();
        return result;
    }
    if (!_state || !_state->currentVolume()) {
        result.code = Code::NoVolume;
        result.message = tr("Open a volume first.");
        return result;
    }
    const auto frame = currentFrame();
    if (!frame) {
        result.code = Code::NoVolume;
        result.message = unavailableReason();
        return result;
    }
    if (draft.id.empty()) {
        draft.id = regions::RegionStore::newId().toStdString();
    }
    if (draft.name.empty()) {
        draft.name = nextName().toStdString();
    }
    if (draft.color.empty()) {
        draft.color = nextColor(_store.regions().size()).toStdString();
    }
    if (draft.author.empty()) {
        QSettings settings(vc3d::settingsFilePath(), QSettings::IniFormat);
        draft.author = settings.value(vc3d::settings::viewer::USERNAME, vc3d::settings::viewer::USERNAME_DEFAULT).toString().toStdString();
    }
    draft.created.clear();
    draft.frame = *frame;
    result = _store.create(std::move(draft));
    emitChanged(result);
    return result;
}

RegionController::Result RegionController::update(regions::Region next, std::optional<std::uint64_t> expectedRevision)
{
    syncDirectory();
    if (const regions::Loaded* existing = find(QString::fromStdString(next.id)); existing && !existing->readOnly) {
        const regions::FrameCheck check = frameCheck(existing->region);
        if (!check.map) {
            Result result;
            result.code = Code::Incompatible;
            result.message =
                tr("Region %1 does not fit this volume: %2").arg(QString::fromStdString(existing->region.name), QString::fromStdString(check.reason));
            return result;
        }
    }
    Result result = _store.update(std::move(next), expectedRevision);
    emitChanged(result);
    return result;
}

RegionController::Result RegionController::remove(const QString& id, std::optional<std::uint64_t> expectedRevision)
{
    syncDirectory();
    if (_drag && _drag->regionId == id) {
        cancelDrag(false);
    }
    Result result = _store.remove(id.toStdString(), expectedRevision);
    emitChanged(result);
    if (result.ok() && result.region) {
        emit statusMessage(tr("Deleted %1 (moved to the regions folder's .trash)").arg(QString::fromStdString(result.region->name)), 4000);
    }
    return result;
}

void RegionController::setActive(const QString& id)
{
    const QString next = (id.isEmpty() || find(id)) ? id : QString();
    if (next == _activeId) {
        return;
    }
    if (_drag && _drag->kind == Drag::Kind::Handle) {
        cancelDrag(false);
    }
    _activeId = next;
    emit activeRegionChanged(_activeId);
}

RegionController::Result RegionController::jumpTo(const QString& id)
{
    Result result;
    const regions::Loaded* loaded = find(id);
    if (!loaded) {
        result.code = Code::NotFound;
        result.message = tr("No region with id %1").arg(id);
        result.param = QStringLiteral("id");
        return result;
    }
    const regions::FrameCheck check = frameCheck(loaded->region);
    if (!check.map) {
        result.code = Code::Incompatible;
        result.message = QString::fromStdString(check.reason);
        return result;
    }
    if (!_viewers) {
        result.code = Code::NoVolume;
        result.message = tr("No viewers");
        return result;
    }
    const cv::Vec3d center = regions::centerViewer(*regions::boxOf(loaded->region), *check.map);
    const cv::Vec3f position(static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2]));
    _viewers->centerFocusAt(position, cv::Vec3f(0.0f, 0.0f, 0.0f), std::string{});
    _viewers->recenterPlaneViewersOn(position);
    setActive(id);
    result.region = loaded->region;
    return result;
}

RegionController::Result RegionController::useAsFocusBox(const QString& id)
{
    Result result;
    const regions::Loaded* loaded = find(id);
    if (!loaded) {
        result.code = Code::NotFound;
        result.message = tr("No region with id %1").arg(id);
        result.param = QStringLiteral("id");
        return result;
    }
    const regions::FrameCheck check = frameCheck(loaded->region);
    if (!check.map) {
        result.code = Code::Incompatible;
        result.message = QString::fromStdString(check.reason);
        return result;
    }
    if (_state) {
        _state->setFocusBounds(regions::focusBounds(*regions::boxOf(loaded->region), *check.map));
        _state->setFocusBoundsEnabled(true);
    }
    emit statusMessage(
        tr("Focus box set to %1: voxels outside it are dimmed in the slice views. Untick BBox in the View dock to turn it off.")
            .arg(QString::fromStdString(loaded->region.name)),
        8000);
    result.region = loaded->region;
    return result;
}

void RegionController::cancelGestures()
{
    _inToolChange = true;
    for (const auto& viewer : _toolViewers) {
        if (viewer) {
            viewer->cancelPointerGesture();
        }
    }
    _inToolChange = false;
    cancelDrag(false);
}

void RegionController::setTool(Tool tool)
{
    if (tool != Tool::None) {
        syncDirectory();
        if (!unavailableReason().isEmpty()) {
            emit statusMessage(unavailableReason(), 5000);
            tool = Tool::None;
        }
    }
    const bool editing = tool == Tool::None ? _editing : false;
    if (tool == _tool && editing == _editing) {
        emit toolChanged();
        return;
    }
    cancelGestures();
    _tool = tool;
    _editing = editing;
    if (_tool == Tool::None && !_editing) {
        clearViewerCursors();
    }
    emit toolChanged();
    emit draftChanged();
}

void RegionController::setEditing(bool editing)
{
    if (editing) {
        syncDirectory();
        if (!unavailableReason().isEmpty()) {
            emit statusMessage(unavailableReason(), 5000);
            editing = false;
        }
    }
    if (editing == _editing && (!editing || _tool == Tool::None)) {
        emit toolChanged();
        return;
    }
    cancelGestures();
    _editing = editing;
    if (editing) {
        _tool = Tool::None;
    }
    if (_tool == Tool::None && !_editing) {
        clearViewerCursors();
    }
    emit toolChanged();
    emit draftChanged();
}

void RegionController::setNewBoxDepth(int voxels)
{
    const int clamped = std::clamp(voxels, 1, kMaxNewBoxDepth);
    if (clamped == _newBoxDepth) {
        return;
    }
    _newBoxDepth = clamped;
    QSettings settings(vc3d::settingsFilePath(), QSettings::IniFormat);
    settings.setValue(kNewBoxDepthKey, _newBoxDepth);
    emit toolChanged();
}

std::optional<regions::Plane> RegionController::planeOf(VolumeViewerBase* viewer)
{
    if (!viewer) {
        return std::nullopt;
    }
    auto* plane = dynamic_cast<PlaneSurface*>(viewer->currentSurface());
    if (!plane) {
        return std::nullopt;
    }
    regions::Plane out;
    const cv::Vec3f origin = plane->origin();
    const cv::Vec3f normal = plane->normal(cv::Vec3f(0, 0, 0));
    const cv::Vec3f bx = plane->basisX();
    const cv::Vec3f by = plane->basisY();
    out.origin = cv::Vec3d(origin[0], origin[1], origin[2]);
    out.normal = cv::Vec3d(normal[0], normal[1], normal[2]);
    out.basisX = cv::Vec3d(bx[0], bx[1], bx[2]);
    out.basisY = cv::Vec3d(by[0], by[1], by[2]);
    return out;
}

bool RegionController::outlineContains(
    VolumeViewerBase* viewer, const regions::Box& box, const regions::FrameMap& map, const regions::Plane& plane, const QPointF& scenePos) const
{
    const auto section = regions::boxCrossSection(box, map, plane);
    if (section.size() < 3) {
        return false;
    }
    QPolygonF polygon;
    for (const auto& p : section) {
        polygon << viewer->volumeToScene(cv::Vec3f(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])));
    }
    return polygon.containsPoint(scenePos, Qt::OddEvenFill);
}

QString RegionController::regionAt(VolumeViewerBase* viewer, const QPointF& scenePos) const
{
    const auto plane = planeOf(viewer);
    if (!plane) {
        return {};
    }
    // The active region wins, then the smallest box under the cursor.
    QString best;
    std::uint64_t bestVoxels = std::numeric_limits<std::uint64_t>::max();
    for (const auto& loaded : _store.regions()) {
        const QString id = QString::fromStdString(loaded.region.id);
        const regions::FrameCheck check = frameCheck(loaded.region);
        if (!check.map) {
            continue;
        }
        std::uint64_t voxels = 0;
        if (const regions::Box* box = regions::boxOf(loaded.region)) {
            if (!outlineContains(viewer, *box, *check.map, *plane, scenePos)) {
                continue;
            }
            voxels = regions::voxelCount(*box);
        }
        if (id == _activeId) {
            return id;
        }
        if (voxels < bestVoxels) {
            bestVoxels = voxels;
            best = id;
        }
    }
    return best;
}

bool RegionController::handlePointer(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event)
{
    using Kind = CChunkedVolumeViewer::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::Cancel:
            if (_drag) {
                cancelDrag(true);
                return true;
            }
            if (_inToolChange) {
                return false;
            }
            if (_tool != Tool::None || _editing) {
                setTool(Tool::None);
                setEditing(false);
                emit statusMessage(tr("Region tools off"), 2000);
                return true;
            }
            return false;
        case Kind::Move:
            if (_drag && _drag->viewer == viewer) {
                updateDrag(event);
                return true;
            }
            if (_tool != Tool::None || _editing) {
                updateHoverCursor(viewer, event);
            }
            return false;
        case Kind::Release:
            if (_drag && _drag->viewer == viewer) {
                finishDrag(event);
            }
            return true;
        case Kind::Press:
        case Kind::DoubleClick:
            return handlePress(viewer, event);
    }
    return false;
}

std::optional<RegionController::Pick> RegionController::pickHandle(
    VolumeViewerBase* viewer, const regions::Plane& plane, regions::PlaneKind kind, const QPointF& scenePos) const
{
    const regions::Loaded* active = find(_activeId);
    if (!active || active->readOnly) {
        return std::nullopt;
    }
    const regions::FrameCheck check = frameCheck(active->region);
    if (!check.map) {
        return std::nullopt;
    }
    std::optional<Pick> pick;
    double bestDistance = kHandlePickPx;
    for (const auto& handle : regions::boxHandles(*regions::boxOf(active->region), *check.map, plane, kind)) {
        const cv::Vec3d& v = handle.viewerPos;
        const QPointF scene = viewer->volumeToScene(cv::Vec3f(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])));
        const double distance = sceneDistance(scene, scenePos);
        if (distance <= bestDistance) {
            bestDistance = distance;
            pick = Pick{handle, *check.map};
        }
    }
    return pick;
}

Qt::CursorShape RegionController::cursorFor(VolumeViewerBase* viewer, const Pick& pick) const
{
    const regions::Handle& handle = pick.box;
    if (handle.kind == regions::HandleKind::Body) {
        return Qt::OpenHandCursor;
    }
    if (handle.kind != regions::HandleKind::Edge || handle.axes[0] < 0) {
        return Qt::SizeAllCursor;
    }
    // An edge moves one axis: pick the split cursor by how that axis lies on
    // screen in this view.
    cv::Vec3d step(0, 0, 0);
    step[handle.axes[0]] = pick.map.regionToViewer();
    const auto sceneOf = [viewer](const cv::Vec3d& v) {
        return viewer->volumeToScene(cv::Vec3f(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])));
    };
    const QPointF a = sceneOf(handle.viewerPos);
    const QPointF b = sceneOf(handle.viewerPos + step);
    return std::abs(b.y() - a.y()) > std::abs(b.x() - a.x()) ? Qt::SplitVCursor : Qt::SplitHCursor;
}

bool RegionController::handlePress(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event)
{
    if (_tool == Tool::None && !_editing) {
        return false;
    }
    if (_drag) {
        cancelDrag(false);
    }
    syncDirectory();
    const auto plane = planeOf(viewer);
    if (!plane) {
        return false;
    }
    const regions::PlaneKind kind = regions::classifyPlane(*plane);
    const cv::Vec3d pressViewer(event.volumePos[0], event.volumePos[1], event.volumePos[2]);
    if (!std::isfinite(pressViewer[0]) || !std::isfinite(pressViewer[1]) || !std::isfinite(pressViewer[2])) {
        return false;
    }
    // The active region's handles come first, so a box edge under the XY
    // view's plane-rotation handle can still be grabbed; any other press on a
    // rotation handle stays the rotation's.
    const std::optional<Pick> pick = pickHandle(viewer, *plane, kind, event.scenePos);
    if (!pick && _foreignHandleQuery && _foreignHandleQuery(viewer, event.scenePos)) {
        return false;
    }

    Drag drag;
    drag.viewer = viewer;
    drag.plane = *plane;
    drag.planeKind = kind;
    drag.pressViewer = pressViewer;
    drag.pressScene = event.scenePos;

    if (pick) {
        const regions::Loaded* active = find(_activeId);
        drag.kind = Drag::Kind::Handle;
        drag.map = pick->map;
        drag.extentMax = regions::frameMaxIndexXyz(active->region.frame);
        drag.regionId = _activeId;
        drag.startRevision = active->region.revision;
        drag.handle = pick->box;
        drag.startBox = *regions::boxOf(active->region);
        _draftBox = drag.startBox;
        _drag = drag;
        setViewerCursor(viewer, Qt::ClosedHandCursor);
        emit draftChanged();
        return true;
    }

    if (_tool != Tool::Box) {
        return false;
    }
    const auto frame = currentFrame();
    const auto map = currentMap();
    if (!frame || !map) {
        emit statusMessage(unavailableReason(), 5000);
        return false;
    }
    drag.kind = Drag::Kind::NewBox;
    drag.map = *map;
    drag.extentMax = regions::frameMaxIndexXyz(*frame);
    _drag = drag;
    _draftBox.reset();
    return true;
}

void RegionController::updateDrag(const CChunkedVolumeViewer::PointerEvent& event)
{
    if (!_drag) {
        return;
    }
    Drag& drag = *_drag;
    if (sceneDistance(event.scenePos, drag.pressScene) >= kClickSlopPx) {
        drag.moved = true;
    }
    if (!drag.moved) {
        return;
    }
    const cv::Vec3d cursor(event.volumePos[0], event.volumePos[1], event.volumePos[2]);
    if (!std::isfinite(cursor[0]) || !std::isfinite(cursor[1]) || !std::isfinite(cursor[2])) {
        return;
    }
    if (drag.kind == Drag::Kind::NewBox) {
        _draftBox = regions::boxFromDrag(drag.pressViewer, cursor, drag.planeKind, drag.plane.origin, _newBoxDepth, drag.map, drag.extentMax);
    } else {
        _draftBox = regions::dragHandle(drag.startBox, drag.handle, drag.pressViewer, cursor, drag.map, drag.extentMax);
    }
    if (_draftBox) {
        emit statusMessage(boxText(*_draftBox), 0);
    }
    emit draftChanged();
}

void RegionController::finishDrag(const CChunkedVolumeViewer::PointerEvent& event)
{
    if (!_drag) {
        return;
    }
    updateDrag(event);
    const Drag drag = *_drag;
    const std::optional<regions::Box> box = _draftBox;
    _drag.reset();
    _draftBox.reset();
    if (drag.viewer) {
        setViewerCursor(drag.viewer.data(), std::nullopt);
    }
    emit draftChanged();

    if (drag.kind == Drag::Kind::NewBox) {
        if (!drag.moved) {
            // A click: make the region under it active, create nothing.
            const QString id = drag.viewer ? regionAt(drag.viewer.data(), drag.pressScene) : QString();
            if (!id.isEmpty()) {
                setActive(id);
                if (const regions::Loaded* loaded = find(id)) {
                    emit statusMessage(
                        QStringLiteral("%1: %2").arg(QString::fromStdString(loaded->region.name), boxText(*regions::boxOf(loaded->region))), 4000);
                }
            }
            return;
        }
        if (!box) {
            emit statusMessage(tr("Drag at least one voxel in both directions to draw a box."), 4000);
            return;
        }
        regions::Region draft;
        draft.shape = *box;
        draft.color.clear();
        const Result result = create(std::move(draft));
        if (!result.ok() || !result.region) {
            return;
        }
        const QString id = QString::fromStdString(result.region->id);
        setActive(id);
        emit statusMessage(tr("Created %1: %2").arg(QString::fromStdString(result.region->name), boxText(*box)), 5000);
        // Edit the new box right away, and bring its depth into the other two
        // slice views.
        setEditing(true);
        centerSlicesOn(*box, drag.map, drag);
        return;
    }

    const regions::Loaded* loaded = find(drag.regionId);
    if (!drag.moved || !loaded) {
        return;
    }
    if (!box || *box == drag.startBox) {
        return;
    }
    regions::Region next = loaded->region;
    next.shape = *box;
    const Result result = update(std::move(next), drag.startRevision);
    if (result.ok() && result.region) {
        emit statusMessage(tr("%1: %2").arg(QString::fromStdString(result.region->name), boxText(*box)), 4000);
    }
}

void RegionController::cancelDrag(bool announce)
{
    if (!_drag && !_draftBox) {
        return;
    }
    if (_drag && _drag->viewer) {
        setViewerCursor(_drag->viewer.data(), std::nullopt);
    }
    _drag.reset();
    _draftBox.reset();
    emit draftChanged();
    if (announce) {
        emit statusMessage(tr("Region drag canceled"), 2000);
    }
}

void RegionController::centerSlicesOn(const regions::Box& box, const regions::FrameMap& map, const Drag& drag)
{
    if (!_viewers) {
        return;
    }
    // The view the box was drawn in keeps its slice; the other two move
    // through the box center, so its depth handles are visible there.
    cv::Vec3d target = regions::centerViewer(box, map);
    const auto axes = regions::inPlaneAxes(drag.planeKind);
    if (axes[0] >= 0) {
        const int normalAxis = 3 - axes[0] - axes[1];
        target[normalAxis] = drag.plane.origin[normalAxis];
    }
    const cv::Vec3f position(static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2]));
    _viewers->centerFocusAt(position, cv::Vec3f(0.0f, 0.0f, 0.0f), std::string{});
}

void RegionController::updateHoverCursor(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event)
{
    const auto plane = planeOf(viewer);
    if (!plane) {
        return;
    }
    const regions::PlaneKind kind = regions::classifyPlane(*plane);
    std::optional<Qt::CursorShape> shape;
    if (const auto pick = pickHandle(viewer, *plane, kind, event.scenePos)) {
        shape = cursorFor(viewer, *pick);
    } else if (_foreignHandleQuery && _foreignHandleQuery(viewer, event.scenePos)) {
        // A press here rotates the plane; keep the viewer's own cursor.
    } else if (_tool == Tool::Box) {
        shape = Qt::CrossCursor;
    }
    setViewerCursor(viewer, shape);
}

void RegionController::setViewerCursor(CChunkedVolumeViewer* viewer, std::optional<Qt::CursorShape> shape)
{
    if (!viewer || !viewer->graphicsView()) {
        return;
    }
    const auto it = _cursors.find(viewer);
    if (!shape) {
        if (it != _cursors.end()) {
            viewer->graphicsView()->unsetCursor();
            _cursors.erase(it);
        }
        return;
    }
    if (it != _cursors.end() && it->second == *shape) {
        return;
    }
    viewer->graphicsView()->setCursor(*shape);
    _cursors[viewer] = *shape;
}

void RegionController::clearViewerCursors()
{
    for (const auto& viewer : _toolViewers) {
        if (viewer && _cursors.count(viewer.data())) {
            setViewerCursor(viewer.data(), std::nullopt);
        }
    }
    _cursors.clear();
}
