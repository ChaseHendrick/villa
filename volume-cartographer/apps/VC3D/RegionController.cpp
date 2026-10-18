#include "RegionController.hpp"

#include "CState.hpp"
#include "OpenDataCoordinateIdentity.hpp"
#include "ProjectFiberPaths.hpp"
#include "RegionExport.hpp"
#include "VCSettings.hpp"
#include "ViewerManager.hpp"
#include "volume_viewers/VolumeViewerBase.hpp"

#include "vc/core/types/Volume.hpp"
#include "vc/core/types/VolumePkg.hpp"
#include "vc/core/util/Logging.hpp"
#include "vc/core/util/PlaneSurface.hpp"

#include <QDir>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPolygonF>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

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

constexpr std::size_t kMaxStatsIds = 64;

QJsonObject toQJson(const regions::Json& json)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(json.dump())).object();
}

std::string volumeLocation(const Volume& volume)
{
    return volume.isRemote() ? volume.remoteUrl() : volume.path().string();
}

QString gib(std::uint64_t bytes)
{
    return QString::number(static_cast<double>(bytes) / (1ull << 30), 'f', 2);
}

struct StatsInput {
    std::vector<regions::Region> regions;
    std::vector<regions::FrameMap> maps;
    std::shared_ptr<Volume> volume;
    std::string volumeId;
    regions::StatsRequest request;
};

struct StatsOutput {
    std::vector<regions::VolumeStats> stats;
    std::string error;
    bool canceled = false;
};

struct ExportInput {
    regions::Region region;
    regions::FrameMap map;
    std::shared_ptr<Volume> volume;
    std::string volumeId;
    std::string location;
    regions::ExportRequest request;
};

}  // namespace

RegionController::RegionController(CState* state, ViewerManager* viewers, QObject* parent)
    : QObject(parent), _state(state), _viewers(viewers)
{
    _jobPool.setMaxThreadCount(1);
    _progressTimer = new QTimer(this);
    _progressTimer->setInterval(200);
    connect(_progressTimer, &QTimer::timeout, this, [this]() {
        if (!_jobFraction || _jobKind.isEmpty()) {
            return;
        }
        const double fraction = _jobFraction->load();
        if (fraction != _lastReportedFraction) {
            _lastReportedFraction = fraction;
            emit jobProgress(_jobKind, fraction);
        }
    });
    {
        QSettings settings(vc3d::settingsFilePath(), QSettings::IniFormat);
        _newBoxDepth = std::clamp(settings.value(kNewBoxDepthKey, kDefaultNewBoxDepth).toInt(), 1, kMaxNewBoxDepth);
    }
    if (_state) {
        connect(_state, &CState::vpkgChanged, this, [this](const std::shared_ptr<VolumePkg>&) {
            cancelJob();
            onProjectChanged();
        });
        connect(_state, &CState::volumeChanged, this, [this](const std::shared_ptr<Volume>&, const std::string&) { onVolumeChanged(); });
        connect(_state, &CState::volumeClosing, this, [this]() {
            cancelGestures();
            cancelJob();
        });
    }
    if (_viewers) {
        _viewers->forEachBaseViewer([this](VolumeViewerBase* viewer) { attachViewer(viewer); });
        connect(_viewers, &ViewerManager::baseViewerCreated, this, [this](VolumeViewerBase* viewer) { attachViewer(viewer); });
    }
    onProjectChanged();
}

RegionController::~RegionController()
{
    if (_jobCancel) {
        _jobCancel->store(true);
    }
    // The cancel flag stops a running job at its next chunk. Workers hold only
    // values and a reference to the volume's chunk cache, so nothing of this
    // object is touched while the pool drains.
    _jobPool.waitForDone();
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
    if (result.ok()) {
        trashStats(id.toStdString());
    }
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

// --- Statistics and crop export ---

std::optional<RegionController::ResolvedVolume> RegionController::resolveVolume(const QString& volumeId) const
{
    if (!_state) {
        return std::nullopt;
    }
    const auto pkg = _state->vpkg();
    ResolvedVolume out;
    if (volumeId.isEmpty() || volumeId.toStdString() == _state->currentVolumeId()) {
        out.volume = _state->currentVolume();
        out.id = _state->currentVolumeId();
    } else if (pkg && pkg->hasVolume(volumeId.toStdString())) {
        try {
            out.volume = pkg->volume(volumeId.toStdString());
        } catch (...) {
            out.volume.reset();
        }
        out.id = volumeId.toStdString();
    }
    if (!out.volume) {
        return std::nullopt;
    }
    out.location = volumeLocation(*out.volume);
    out.frame = vc3d::opendata::annotationFrameForVolume(pkg.get(), out.id, *out.volume);
    if (pkg && !out.id.empty()) {
        try {
            out.space = vc3d::opendata::coordinateSpaceTag(pkg->volumeTags(out.id));
        } catch (...) {
            out.space.clear();
        }
    }
    return out;
}

std::vector<RegionController::VolumeChoice> RegionController::compatibleVolumes(const QString& regionId) const
{
    std::vector<VolumeChoice> choices;
    const regions::Loaded* loaded = find(regionId);
    if (!loaded || !_state) {
        return choices;
    }
    std::vector<QString> ids;
    if (_state->currentVolume()) {
        ids.push_back(QString::fromStdString(_state->currentVolumeId()));
    }
    if (const auto pkg = _state->vpkg()) {
        for (const auto& id : pkg->volumeIDs()) {
            const QString qid = QString::fromStdString(id);
            if (std::find(ids.begin(), ids.end(), qid) == ids.end()) {
                ids.push_back(qid);
            }
        }
    }
    for (const QString& id : ids) {
        const auto volume = resolveVolume(id);
        if (!volume) {
            continue;
        }
        const regions::FrameCheck check = regions::mapRegionFrame(loaded->region.frame, volume->frame, volume->space);
        if (!check.map) {
            continue;
        }
        choices.push_back({id, volume->id == _state->currentVolumeId(), *check.map});
    }
    return choices;
}

QStringList RegionController::compatibleVolumeIds(const QString& regionId) const
{
    QStringList ids;
    for (const auto& choice : compatibleVolumes(regionId)) {
        ids << choice.id;
    }
    return ids;
}

std::vector<RegionController::LevelEstimate> RegionController::levelEstimates(const QString& regionId, const QString& volumeId) const
{
    std::vector<LevelEstimate> estimates;
    const regions::Loaded* loaded = find(regionId);
    const auto volume = resolveVolume(volumeId);
    if (!loaded || !volume) {
        return estimates;
    }
    const regions::FrameCheck check = regions::mapRegionFrame(loaded->region.frame, volume->frame, volume->space);
    if (!check.map) {
        return estimates;
    }
    const std::uint64_t voxelBytes = volume->volume->dtype() == vc::render::ChunkDtype::UInt16 ? 2 : 1;
    for (const auto& [level, voxels] : regions::levelVoxelCounts(*volume->volume, *regions::boxOf(loaded->region), *check.map)) {
        estimates.push_back({level, voxels, voxels * voxelBytes});
    }
    return estimates;
}

int RegionController::autoLevel(const std::vector<LevelEstimate>& estimates)
{
    const regions::StatsRequest defaults;
    for (const auto& estimate : estimates) {
        if (estimate.voxels <= defaults.voxelBudget) {
            return estimate.level;
        }
    }
    return estimates.empty() ? 0 : estimates.back().level;
}

RegionController::Result RegionController::checkMeasurable(const QString& id, const ResolvedVolume& volume, regions::FrameMap* map) const
{
    Result result;
    const regions::Loaded* loaded = find(id);
    if (!loaded) {
        result.code = Code::NotFound;
        result.message = tr("No region with id %1").arg(id);
        result.param = QStringLiteral("ids");
        return result;
    }
    if (loaded->readOnly) {
        result.code = Code::ReadOnly;
        result.message =
            tr("Region %1 is read-only: %2").arg(QString::fromStdString(loaded->region.name), QString::fromStdString(loaded->readOnlyReason));
        return result;
    }
    const regions::FrameCheck check = regions::mapRegionFrame(loaded->region.frame, volume.frame, volume.space);
    if (!check.map) {
        result.code = Code::Incompatible;
        result.message =
            tr("Region %1 does not fit volume %2: %3")
                .arg(QString::fromStdString(loaded->region.name), QString::fromStdString(volume.id), QString::fromStdString(check.reason));
        return result;
    }
    *map = *check.map;
    return result;
}

void RegionController::beginJob(const QString& kind, const QString& label, const QStringList& ids)
{
    _jobKind = kind;
    _jobLabel = label;
    _jobCancel = std::make_shared<std::atomic<bool>>(false);
    _jobFraction = std::make_shared<std::atomic<double>>(0.0);
    _lastReportedFraction = -1.0;
    _progressTimer->start();
    emit jobStarted(kind, label, ids);
}

void RegionController::endJob()
{
    _progressTimer->stop();
    _jobKind.clear();
    _jobLabel.clear();
    _jobCancel.reset();
    _jobFraction.reset();
}

bool RegionController::cancelJob()
{
    if (!_jobCancel) {
        return false;
    }
    _jobCancel->store(true);
    return true;
}

RegionController::Result RegionController::startStats(const StatsJob& job)
{
    syncDirectory();
    Result result;
    const auto refuse = [&result](Code code, QString message, QString param = {}) {
        result.code = code;
        result.message = std::move(message);
        result.param = std::move(param);
        return result;
    };
    if (directory().isEmpty()) {
        return refuse(Code::NoProject, unavailableReason());
    }
    if (jobRunning()) {
        return refuse(Code::JobRunning, tr("A region job is already running (%1)").arg(_jobLabel));
    }
    if (job.ids.isEmpty() || static_cast<std::size_t>(job.ids.size()) > kMaxStatsIds) {
        return refuse(Code::Invalid, tr("Give 1 to %1 region ids").arg(kMaxStatsIds), QStringLiteral("ids"));
    }
    if (job.threshold && !std::isfinite(*job.threshold)) {
        return refuse(Code::Invalid, tr("The threshold must be a finite number"), QStringLiteral("threshold"));
    }
    if (job.percentiles.size() > 64) {
        return refuse(Code::Invalid, tr("Give at most 64 percentiles"), QStringLiteral("percentiles"));
    }
    for (const double p : job.percentiles) {
        if (!regions::isValidPercentile(p)) {
            return refuse(Code::Invalid, tr("Percentiles must be in [0, 100] with at most 4 decimals"), QStringLiteral("percentiles"));
        }
    }
    const auto volume = resolveVolume(job.volumeId);
    if (!volume) {
        if (job.volumeId.isEmpty()) {
            return refuse(Code::NoVolume, tr("Open a volume first."));
        }
        return refuse(Code::NotFound, tr("No loaded volume with id %1 in this project").arg(job.volumeId), QStringLiteral("volumeId"));
    }
    const auto dtype = volume->volume->dtype();
    if (dtype != vc::render::ChunkDtype::UInt8 && dtype != vc::render::ChunkDtype::UInt16) {
        return refuse(Code::Incompatible, tr("Statistics support uint8 and uint16 volumes only"));
    }
    if (job.level >= 0 && !volume->volume->hasScaleLevel(job.level)) {
        return refuse(Code::Invalid, tr("Level %1 is not stored in volume %2").arg(job.level).arg(QString::fromStdString(volume->id)), QStringLiteral("level"));
    }

    StatsInput input;
    for (const QString& id : job.ids) {
        regions::FrameMap map;
        if (Result check = checkMeasurable(id, *volume, &map); !check.ok()) {
            return check;
        }
        input.regions.push_back(find(id)->region);
        input.maps.push_back(map);
    }
    const std::uint64_t voxelBytes = dtype == vc::render::ChunkDtype::UInt16 ? 2 : 1;
    for (std::size_t i = 0; i < input.regions.size(); ++i) {
        const regions::Box& box = *regions::boxOf(input.regions[i]);
        const int level = job.level >= 0 ? job.level : regions::chooseLevel(*volume->volume, box, input.maps[i], input.request.voxelBudget);
        const auto range = regions::boxIndexRange(box, input.maps[i], level, volume->volume->shape(level));
        const QString name = QString::fromStdString(input.regions[i].name);
        if (range.empty()) {
            return refuse(Code::Invalid, tr("Region %1 is outside volume %2").arg(name, QString::fromStdString(volume->id)), QStringLiteral("ids"));
        }
        if (range.count() * voxelBytes > regions::kMaxReadBytes) {
            return refuse(
                Code::Invalid,
                tr("Region %1 has %2 GiB of voxels at level %3; statistics above %4 GiB are refused (choose a coarser level)")
                    .arg(name, gib(range.count() * voxelBytes))
                    .arg(level)
                    .arg(regions::kMaxReadBytes >> 30),
                QStringLiteral("level"));
        }
    }
    input.volume = volume->volume;
    input.volumeId = volume->id;
    input.request.level = job.level;
    input.request.threshold = job.threshold;
    input.request.percentiles = job.percentiles;
    input.request.threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);

    const QString label = job.ids.size() == 1 ? tr("Statistics of %1").arg(QString::fromStdString(input.regions.front().name))
                                              : tr("Statistics of %1 regions").arg(job.ids.size());
    beginJob(QString::fromLatin1(kStatsJobKind), label, job.ids);
    const auto cancel = _jobCancel;
    const auto fraction = _jobFraction;
    const std::string location = volume->location;
    const std::filesystem::path directoryAtStart = _store.directory();

    auto* watcher = new QFutureWatcher<StatsOutput>(this);
    connect(watcher, &QFutureWatcher<StatsOutput>::finished, this, [this, watcher, input, location, directoryAtStart]() {
        watcher->deleteLater();
        const StatsOutput output = watcher->result();
        const QString kind = QString::fromLatin1(kStatsJobKind);
        endJob();
        if (!output.error.empty()) {
            emit jobFinished(kind, false, tr("Statistics failed: %1").arg(QString::fromStdString(output.error)), {}, {});
            return;
        }
        if (output.canceled) {
            emit jobFinished(kind, false, tr("Statistics canceled; nothing was saved."), {}, {});
            return;
        }
        if (_store.directory() != directoryAtStart) {
            emit jobFinished(kind, false, tr("The project changed while the statistics ran; nothing was saved."), {}, {});
            return;
        }
        const std::string created = regions::RegionStore::nowUtc();
        regions::Json docs = regions::Json::array();
        QStringList outputPaths;
        QStringList skipped;
        for (std::size_t i = 0; i < output.stats.size(); ++i) {
            const regions::Region& measured = input.regions[i];
            const regions::Loaded* current = find(QString::fromStdString(measured.id));
            if (!current || current->region.revision != measured.revision) {
                skipped << QString::fromStdString(measured.name);
                continue;
            }
            const regions::Json doc = regions::statsJson(output.stats[i], measured, input.maps[i], location, created);
            const QString dir = statsDirectory(QString::fromStdString(measured.id));
            QDir().mkpath(dir);
            const QString path = QDir(dir).filePath(QString::fromStdString(regions::statsFileName(output.stats[i].volumeId, output.stats[i].level)));
            if (const QString error = regions::writeFileAtomic(path.toStdU16String(), doc.dump(2) + "\n"); !error.isEmpty()) {
                emit jobFinished(kind, false, error, {}, outputPaths);
                return;
            }
            docs.push_back(doc);
            outputPaths << path;
            emit savedStatsChanged(QString::fromStdString(measured.id));
        }
        regions::Json body = regions::Json::object();
        body["stats"] = docs;
        body["outputPaths"] = regions::Json::array();
        for (const QString& path : outputPaths) {
            body["outputPaths"].push_back(path.toStdString());
        }
        QString message = tr("Measured %n region(s)", nullptr, static_cast<int>(outputPaths.size()));
        if (!skipped.isEmpty()) {
            message += tr("; not saved because the region changed meanwhile: %1").arg(skipped.join(QStringLiteral(", ")));
        }
        emit jobFinished(kind, skipped.isEmpty(), message, toQJson(body), outputPaths);
    });
    watcher->setFuture(QtConcurrent::run(&_jobPool, [input, cancel, fraction]() {
        StatsOutput output;
        const std::size_t n = input.regions.size();
        for (std::size_t i = 0; i < n; ++i) {
            regions::Progress progress;
            progress.cancel = cancel.get();
            progress.report = [fraction, i, n](double f) { fraction->store((static_cast<double>(i) + f) / static_cast<double>(n)); };
            std::string error;
            auto stats =
                regions::computeStats(*input.volume, input.volumeId, *regions::boxOf(input.regions[i]), input.maps[i], input.request, progress, &error);
            if (!stats) {
                output.error = error;
                return output;
            }
            if (stats->canceled) {
                output.canceled = true;
                return output;
            }
            output.stats.push_back(std::move(*stats));
        }
        fraction->store(1.0);
        return output;
    }));
    result.region = input.regions.front();
    return result;
}

RegionController::Result RegionController::startExport(const QString& id, const QString& volumeId, int level, const QString& outDir)
{
    syncDirectory();
    Result result;
    const auto refuse = [&result](Code code, QString message, QString param = {}) {
        result.code = code;
        result.message = std::move(message);
        result.param = std::move(param);
        return result;
    };
    if (directory().isEmpty()) {
        return refuse(Code::NoProject, unavailableReason());
    }
    if (jobRunning()) {
        return refuse(Code::JobRunning, tr("A region job is already running (%1)").arg(_jobLabel));
    }
    const auto volume = resolveVolume(volumeId);
    if (!volume) {
        return volumeId.isEmpty()
                   ? refuse(Code::NoVolume, tr("Open a volume first."))
                   : refuse(Code::NotFound, tr("No loaded volume with id %1 in this project").arg(volumeId), QStringLiteral("volumeId"));
    }
    if (!volume->volume->hasScaleLevel(level)) {
        return refuse(Code::Invalid, tr("Level %1 is not stored in volume %2").arg(level).arg(QString::fromStdString(volume->id)), QStringLiteral("level"));
    }
    ExportInput input;
    if (Result check = checkMeasurable(id, *volume, &input.map); !check.ok()) {
        return check;
    }
    input.region = find(id)->region;
    input.volume = volume->volume;
    input.volumeId = volume->id;
    input.location = volume->location;
    input.request.level = level;
    input.request.created = regions::RegionStore::nowUtc();
    {
        QSettings settings(vc3d::settingsFilePath(), QSettings::IniFormat);
        input.request.author = settings.value(vc3d::settings::viewer::USERNAME, vc3d::settings::viewer::USERNAME_DEFAULT).toString().toStdString();
    }
    input.request.outDir = outDir.isEmpty() ? regions::defaultExportDir(_store.directory(), input.region.id, input.request.created)
                                            : std::filesystem::path(outDir.toStdU16String());
    const std::uint64_t bytes = regions::estimateExportBytes(*input.volume, *regions::boxOf(input.region), input.map, level);
    if (bytes == 0) {
        return refuse(
            Code::Invalid, tr("Region %1 is outside volume %2").arg(QString::fromStdString(input.region.name), QString::fromStdString(volume->id)));
    }
    if (bytes > input.request.maxBytes) {
        return refuse(
            Code::Invalid,
            tr("The crop would be %1 GiB at level %2; exports above %3 GiB are refused (choose a coarser level)")
                .arg(static_cast<double>(bytes) / (1ull << 30), 0, 'f', 2)
                .arg(level)
                .arg(static_cast<double>(input.request.maxBytes) / (1ull << 30), 0, 'f', 0),
            QStringLiteral("level"));
    }

    const QString label = tr("Export of %1 at level %2").arg(QString::fromStdString(input.region.name)).arg(level);
    beginJob(QString::fromLatin1(kExportJobKind), label, {id});
    const auto cancel = _jobCancel;
    const auto fraction = _jobFraction;
    auto* watcher = new QFutureWatcher<regions::ExportResult>(this);
    connect(watcher, &QFutureWatcher<regions::ExportResult>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        const regions::ExportResult output = watcher->result();
        const QString kind = QString::fromLatin1(kExportJobKind);
        endJob();
        if (output.canceled) {
            emit jobFinished(kind, false, tr("Export canceled; the partial crop was removed."), {}, {});
            return;
        }
        if (!output.error.empty()) {
            emit jobFinished(kind, false, tr("Export failed: %1").arg(QString::fromStdString(output.error)), {}, {});
            return;
        }
        const QString dir = vc3d::pathToQString(output.dir);
        regions::Json body = regions::Json::object();
        body["manifest"] = output.manifest;
        body["outputPaths"] = regions::Json::array({output.dir.string()});
        emit jobFinished(kind, true, tr("Saved the crop to %1").arg(dir), toQJson(body), {dir});
    });
    watcher->setFuture(QtConcurrent::run(&_jobPool, [input, cancel, fraction]() {
        regions::Progress progress;
        progress.cancel = cancel.get();
        progress.report = [fraction](double f) { fraction->store(f); };
        return regions::exportCrop(*input.volume, input.volumeId, input.location, input.region, input.map, input.request, progress);
    }));
    result.region = input.region;
    return result;
}

QString RegionController::statsDirectory(const QString& regionId) const
{
    if (_store.directory().empty() || !regions::isValidId(regionId.toStdString())) {
        return {};
    }
    return vc3d::pathToQString(_store.directory() / "stats" / regionId.toStdString());
}

std::vector<RegionController::SavedStats> RegionController::savedStats(const QString& regionId) const
{
    std::vector<SavedStats> out;
    const regions::Loaded* loaded = find(regionId);
    const QString dir = statsDirectory(regionId);
    if (!loaded || dir.isEmpty()) {
        return out;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir.toStdU16String()), ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const auto doc = regions::readJsonFile(entry.path(), nullptr);
        if (!doc || !doc->is_object() || doc->value("schema", std::string()) != "vc3d.region_stats") {
            continue;
        }
        SavedStats saved;
        saved.path = vc3d::pathToQString(entry.path());
        saved.level = doc->value("level", 0);
        if (doc->contains("volume") && (*doc)["volume"].is_object()) {
            saved.volumeId = QString::fromStdString((*doc)["volume"].value("id", std::string()));
        }
        saved.stale = doc->value("region_revision", std::uint64_t{0}) != loaded->region.revision;
        saved.document = *doc;
        out.push_back(std::move(saved));
    }
    std::sort(out.begin(), out.end(), [](const SavedStats& a, const SavedStats& b) {
        const std::string ca = a.document.value("created", std::string());
        const std::string cb = b.document.value("created", std::string());
        return ca != cb ? ca > cb : a.path < b.path;
    });
    return out;
}

void RegionController::trashStats(const std::string& id)
{
    const std::filesystem::path dir = _store.directory();
    if (dir.empty() || !regions::isValidId(id)) {
        return;
    }
    const std::filesystem::path from = dir / "stats" / id;
    const std::filesystem::path to = dir / ".trash" / "stats" / id;
    std::error_code ec;
    if (!std::filesystem::exists(from, ec)) {
        return;
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::remove_all(to, ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (ec) {
        Logger()->warn("Regions: could not move statistics {} to {}: {}", from.string(), to.string(), ec.message());
    }
}
