#pragma once

// Regions of interest in VC3D: owns the project's region files (through
// RegionStore), the 3D Box, Ray Arc and Edit tools in the slice views, the
// statistics and crop-export jobs, and the region actions (jump to, use as
// focus box).

#include "RegionModel.hpp"
#include "RegionStats.hpp"
#include "RegionStore.hpp"
#include "volume_viewers/CChunkedVolumeViewer.hpp"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CState;
class QTimer;
class Volume;
class ViewerManager;
class VolumeViewerBase;

class RegionController : public QObject
{
    Q_OBJECT

public:
    // True when a press at this scene position belongs to another overlay's
    // handle (the XY view's plane-rotation handles), which then keeps it.
    using HandleQuery = std::function<bool(VolumeViewerBase*, const QPointF&)>;
    using Code = vc3d::regions::Code;
    using Result = vc3d::regions::Result;

    RegionController(CState* state, ViewerManager* viewers, QObject* parent = nullptr);
    ~RegionController() override;

    void setForeignHandleQuery(HandleQuery query) { _foreignHandleQuery = std::move(query); }

    // Re-derives the regions folder from the project path (it changes when a
    // project is first saved or saved under a new name) and reloads when it
    // moved. Returns true when it changed.
    bool syncDirectory();
    QString directory() const;
    const std::vector<vc3d::regions::Loaded>& regions() const { return _store.regions(); }
    const std::vector<vc3d::regions::LoadError>& loadErrors() const { return _store.loadErrors(); }
    const vc3d::regions::Loaded* find(const QString& id) const;
    vc3d::regions::FrameCheck frameCheck(const vc3d::regions::Region& region) const;
    // Empty when the tools can be used, else the message the dock shows.
    QString unavailableReason() const;
    // The frame and map a region drawn on the current volume gets.
    std::optional<vc3d::regions::Frame> currentFrame() const;
    std::optional<vc3d::regions::FrameMap> currentMap() const;
    QString currentVolumeId() const;

    // Fills id, author, created, color, name and frame when empty. A ray arc
    // must compile over its frame.
    Result create(vc3d::regions::Region draft);
    Result update(vc3d::regions::Region next, std::optional<std::uint64_t> expectedRevision);
    // Moves the region file (and its saved statistics) to the regions
    // folder's .trash.
    Result remove(const QString& id, std::optional<std::uint64_t> expectedRevision);

    void setActive(const QString& id);
    QString activeId() const { return _activeId; }
    // Moves the focus point to the region's center, so the slice planes pass
    // through it, and centers the slice views on it. Moving the focus point
    // also recenters the segmentation view, as any focus change does.
    Result jumpTo(const QString& id);
    // Sets the session focus box to exactly the region's box (a ray arc's
    // bounding box).
    Result useAsFocusBox(const QString& id);

    enum class Tool { None, Box, RayArc };
    void setTool(Tool tool);
    Tool tool() const { return _tool; }
    void setEditing(bool editing);
    bool editing() const { return _editing; }
    void setNewBoxDepth(int voxels);
    int newBoxDepth() const { return _newBoxDepth; }
    // During a drag, for the overlay: the box being drawn or edited, and the id
    // of the region being edited (empty for a new box).
    std::optional<vc3d::regions::Box> draftBox() const { return _draftBox; }
    // A ray arc being drawn (after its first click) or edited.
    std::optional<vc3d::regions::RayArc> draftArc() const { return _draftArc; }
    QString draftRegionId() const { return _drag ? _drag->regionId : QString(); }

    static std::optional<vc3d::regions::Plane> planeOf(VolumeViewerBase* viewer);
    static QString nextColor(std::size_t index);

    // --- Ray arcs ---

    // The project umbilicus as new ray arcs embed it, framed in the current
    // volume's annotation frame; unavailable with the reason when there is
    // none or it was refused.
    struct UmbilicusState {
        bool available = false;
        QString message;
        vc3d::regions::UmbilicusRef ref;
    };
    const UmbilicusState& umbilicus() const { return _umbilicus; }
    std::shared_ptr<const vc3d::regions::CompiledArc> currentUmbilicusCompiled() const { return _umbilicusCompiled; }
    // New arcs run from the first click to the second in increasing theta
    // (true) or decreasing; their seam is one of +x, -x, +y, -y.
    void setArcIncreasing(bool increasing);
    bool arcIncreasing() const { return _arcIncreasing; }
    void setArcSeam(const QString& seam);
    QString arcSeam() const { return _arcSeam; }
    // Puts the project umbilicus into `arc` with the given seam, its control
    // points carried from the current volume's annotation frame into the
    // region frame `map` belongs to (a region's map onto the current volume).
    // False, changing nothing, when there is no usable umbilicus
    // (umbilicus().message says why).
    bool embedProjectUmbilicus(vc3d::regions::RayArc& arc, const vc3d::regions::FrameMap& map, const std::string& seam) const;
    // Re-embeds the project umbilicus into a ray arc (one revision).
    Result refreshUmbilicus(const QString& id, std::optional<std::uint64_t> expectedRevision);
    // The compiled umbilicus of a ray arc region, cached per revision; null
    // for boxes and for arcs whose umbilicus cannot be built.
    std::shared_ptr<const vc3d::regions::CompiledArc> compiledArc(const vc3d::regions::Region& region) const;
    // Region-frame bounds of any shape, inclusive; nullopt when unknown.
    std::optional<std::array<cv::Vec3d, 2>> regionBounds(const vc3d::regions::Region& region) const;

    // --- Statistics and crop export (one job at a time) ---

    // A project volume a region can be measured on.
    struct VolumeChoice {
        QString id;
        bool current = false;
        vc3d::regions::FrameMap map;
    };
    // The loaded project volumes the region fits, the current volume first.
    std::vector<VolumeChoice> compatibleVolumes(const QString& regionId) const;
    QStringList compatibleVolumeIds(const QString& regionId) const;

    struct LevelEstimate {
        int level = 0;
        std::uint64_t voxels = 0;
        std::uint64_t bytes = 0;
    };
    // Voxel and byte counts of the region at every stored level of a volume
    // (empty volume id: the current one), finest first, and the level Auto
    // picks.
    std::vector<LevelEstimate> levelEstimates(const QString& regionId, const QString& volumeId) const;
    // The level Auto picks from those estimates: the finest within the
    // statistics voxel budget, else the coarsest.
    static int autoLevel(const std::vector<LevelEstimate>& estimates);
    int autoLevel(const QString& regionId, const QString& volumeId) const { return autoLevel(levelEstimates(regionId, volumeId)); }

    struct StatsJob {
        QStringList ids;
        QString volumeId;  // empty: the current volume
        int level = -1;    // -1: Auto
        std::optional<double> threshold;
        std::vector<double> percentiles{0.5, 1, 5, 25, 50, 75, 95, 99, 99.5};
    };
    // Validates everything, then starts the job; the result arrives through
    // jobFinished(). Refused with a typed code (NotFound, ReadOnly,
    // Incompatible, Invalid with param, JobRunning, NoProject, NoVolume); a
    // region with more than regions::kMaxReadBytes of voxels at the level is
    // refused.
    Result startStats(const StatsJob& job);
    // Saves the region's voxels at `level` as a local zarr crop. An empty
    // `outDir` uses the default folder under the regions folder.
    Result startExport(const QString& id, const QString& volumeId, int level, const QString& outDir);
    bool cancelJob();
    bool jobRunning() const { return !_jobKind.isEmpty(); }
    QString jobKind() const { return _jobKind; }
    QString jobLabel() const { return _jobLabel; }

    // Saved statistics of a region, one per volume and level, newest first.
    struct SavedStats {
        QString volumeId;
        int level = 0;
        bool stale = false;
        QString path;
        vc3d::regions::Json document;
    };
    std::vector<SavedStats> savedStats(const QString& regionId) const;
    QString statsDirectory(const QString& regionId) const;

    static constexpr const char* kStatsJobKind = "regions.stats";
    static constexpr const char* kExportJobKind = "regions.export";

signals:
    void regionsChanged();
    void activeRegionChanged(const QString& id);
    void toolChanged();
    void draftChanged();
    void statusMessage(const QString& text, int timeoutMs);
    void jobStarted(const QString& kind, const QString& label, const QStringList& ids);
    void jobProgress(const QString& kind, double fraction);
    // `result` is {"stats": [stats documents], "outputPaths": [...]} for
    // statistics and {"manifest": ..., "outputPaths": [...]} for an export.
    void jobFinished(const QString& kind, bool success, const QString& message, const QJsonObject& result, const QStringList& outputPaths);
    void savedStatsChanged(const QString& regionId);

private:
    struct Drag {
        enum class Kind { NewBox, Handle };
        Kind kind = Kind::NewBox;
        QPointer<CChunkedVolumeViewer> viewer;
        vc3d::regions::Plane plane;
        vc3d::regions::PlaneKind planeKind = vc3d::regions::PlaneKind::Tilted;
        cv::Vec3d pressViewer{0, 0, 0};
        QPointF pressScene;
        bool moved = false;
        vc3d::regions::FrameMap map;
        cv::Vec3i extentMax{0, 0, 0};
        // Handle drags only.
        QString regionId;
        vc3d::regions::Box startBox;
        std::uint64_t startRevision = 0;
        vc3d::regions::Handle handle;
        // Ray arc handle drags.
        std::optional<vc3d::regions::ArcHandleKind> arcHandle;
        vc3d::regions::RayArc startArc;
        std::shared_ptr<const vc3d::regions::CompiledArc> compiled;
    };

    // The active region's handle under a scene point, within the pick radius:
    // a ray arc's radius and angle handles on XY, a box's corner, edge and
    // center handles on axis planes and its z edges on vertical planes.
    struct Pick {
        std::optional<vc3d::regions::Handle> box;
        std::optional<vc3d::regions::ArcHandleKind> arc;
        vc3d::regions::FrameMap map;
    };
    std::optional<Pick> pickHandle(VolumeViewerBase* viewer, const vc3d::regions::Plane& plane, vc3d::regions::PlaneKind kind, const QPointF& scenePos) const;
    Qt::CursorShape cursorFor(VolumeViewerBase* viewer, const Pick& pick) const;
    std::optional<vc3d::regions::ShapeSpec> specOf(const vc3d::regions::Region& region) const;
    Result checkCompiles(const vc3d::regions::Region& region) const;
    bool handlePointer(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event);
    bool handlePress(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event);
    void updateDrag(const CChunkedVolumeViewer::PointerEvent& event);
    void finishDrag(const CChunkedVolumeViewer::PointerEvent& event);
    void cancelDrag(bool announce);
    void updateHoverCursor(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event);
    void setViewerCursor(CChunkedVolumeViewer* viewer, std::optional<Qt::CursorShape> shape);
    void clearViewerCursors();
    QString regionAt(VolumeViewerBase* viewer, const QPointF& scenePos) const;
    bool outlineContains(VolumeViewerBase* viewer, const vc3d::regions::Box& box, const vc3d::regions::FrameMap& map, const vc3d::regions::Plane& plane, const QPointF& scenePos) const;
    void centerSlicesOn(const vc3d::regions::Box& box, const vc3d::regions::FrameMap& map, const Drag& drag);

    struct ResolvedVolume {
        std::shared_ptr<Volume> volume;
        std::string id;
        std::string location;
        vc3d::annotation::AnnotationFrame frame;
        std::string space;
    };
    std::optional<ResolvedVolume> resolveVolume(const QString& volumeId) const;
    Result checkMeasurable(const QString& id, const ResolvedVolume& volume, vc3d::regions::FrameMap* map) const;
    void beginJob(const QString& kind, const QString& label, const QStringList& ids);
    void endJob();
    void trashStats(const std::string& id);

    void attachViewer(VolumeViewerBase* viewer);
    void refreshUmbilicusState();
    bool handleArcPress(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event, const vc3d::regions::Plane& plane);
    void updateArcPreview(CChunkedVolumeViewer* viewer, const CChunkedVolumeViewer::PointerEvent& event);
    void clearArcDraft();
    std::optional<cv::Vec3d> regionPointOf(const CChunkedVolumeViewer::PointerEvent& event, const vc3d::regions::FrameMap& map) const;
    void cancelGestures();
    void onProjectChanged();
    void onVolumeChanged();
    void refreshVolumeFrame();
    void rebuildFrameChecks();
    void emitChanged(const Result& result);
    QString nextName() const;

    CState* _state = nullptr;
    ViewerManager* _viewers = nullptr;
    HandleQuery _foreignHandleQuery;
    vc3d::regions::RegionStore _store;

    // The current volume, as regions see it.
    vc3d::annotation::AnnotationFrame _volumeFrame;
    std::string _volumeSpace;
    vc3d::regions::Json _volumeIdentity = vc3d::regions::Json::object();
    std::unordered_map<std::string, vc3d::regions::FrameCheck> _frameChecks;

    QString _activeId;
    Tool _tool = Tool::None;
    bool _editing = false;
    bool _inToolChange = false;
    int _newBoxDepth = 64;
    std::optional<Drag> _drag;
    std::optional<vc3d::regions::Box> _draftBox;
    std::optional<vc3d::regions::RayArc> _draftArc;
    // The first click of a ray arc being drawn, region frame, and its viewer.
    std::optional<cv::Vec3d> _arcFirst;
    QPointer<CChunkedVolumeViewer> _arcViewer;
    bool _arcIncreasing = true;
    QString _arcSeam{QStringLiteral("+x")};
    UmbilicusState _umbilicus;
    // The project umbilicus compiled over the current volume's annotation
    // frame, for drawing new arcs.
    std::shared_ptr<const vc3d::regions::CompiledArc> _umbilicusCompiled;
    void compileCurrentUmbilicus();
    mutable std::unordered_map<std::string, std::pair<std::uint64_t, std::shared_ptr<const vc3d::regions::CompiledArc>>> _compiledArcs;

    std::vector<QPointer<CChunkedVolumeViewer>> _toolViewers;
    std::unordered_map<CChunkedVolumeViewer*, Qt::CursorShape> _cursors;

    // The running job. Workers capture values only and report progress
    // through the shared atomic, which _progressTimer polls on the GUI thread.
    QThreadPool _jobPool;
    QString _jobKind;
    QString _jobLabel;
    std::shared_ptr<std::atomic<bool>> _jobCancel;
    std::shared_ptr<std::atomic<double>> _jobFraction;
    double _lastReportedFraction = -1.0;
    QTimer* _progressTimer = nullptr;
};
