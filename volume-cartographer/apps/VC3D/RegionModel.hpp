#pragma once

// Regions of interest marked in VC3D's slice views. Everything in this header
// is free of Qt so that the geometry and the file format can be tested and
// reused without a GUI.
//
// Coordinates come in three systems (docs/regions.md has the full rules):
//   - region frame: integer voxels of the annotation frame the region was
//     drawn in (the scan's level-0 source grid, the frame fibers use);
//   - viewer frame: level-0 coordinates of the open store, the frame of the
//     slice planes, CState::focusBounds and the agent bridge;
//   - store level L: index i of level L is the viewer point i * 2^L.
// A region point r is the viewer point r * k, where k = 2^log2RegionToViewer
// comes from mapRegionFrame().

#include "AnnotationFrame.hpp"

#include "vc/core/util/Rect3D.hpp"

#include <nlohmann/json.hpp>
#include <opencv2/core/matx.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vc3d::regions
{

using Json = nlohmann::ordered_json;

inline constexpr std::string_view kSchema = "vc3d.region";
inline constexpr int kVersion = 1;
inline constexpr std::string_view kFileSuffix = ".region.json";

// Inclusive voxel-center bounds in the region frame, x y z order.
struct Box {
    cv::Vec3i min{0, 0, 0};
    cv::Vec3i max{0, 0, 0};
    bool operator==(const Box&) const = default;
};

struct Frame {
    // coordinate_base_shape_zyx: the annotation extent the region was drawn in.
    std::array<long long, 3> baseShapeZyx{0, 0, 0};
    std::optional<double> voxelSizeUm;
    // Provenance only; never used to decide compatibility.
    std::string volumeId;
    // vc_open_data_coordinate_space; may be empty.
    std::string coordinateSpace;
    // The vc_open_data_* keys exactly as fiber exports write them.
    Json coordinateIdentity = Json::object();
};

// --- Ray arcs ---

// The project umbilicus as a ray arc embeds it: control points already
// carried into the region frame, so the arc's voxels stay fixed when the
// project's umbilicus file is refined later ("Refresh umbilicus" re-embeds).
struct UmbilicusRef {
    std::string sourcePath;
    double scale = 1.0;
    std::string scaleSource{"stamped_dimensions"};  // stamped_dimensions | stamped_voxel_size | inferred_from_grid
    std::string seam{"+x"};                         // +x | -x | +y | -y
    std::vector<cv::Vec3d> controlPoints;           // region frame
    bool operator==(const UmbilicusRef&) const = default;
};

// An annular sector around the umbilicus over a z range: the region points
// with z_min <= z <= z_max, radius_min <= r_xy <= radius_max and
// fmod(theta - theta_start + 360, 360) <= theta_sweep.
struct RayArc {
    double thetaStartDeg = 0.0;   // [0, 360), from the seam toward +y
    double thetaSweepDeg = 90.0;  // (0, 360]
    double radiusMin = 0.0;       // voxels, XY distance from the umbilicus
    double radiusMax = 1.0;
    int zMin = 0;
    int zMax = 0;
    UmbilicusRef umbilicus;
    bool operator==(const RayArc&) const = default;
};

inline constexpr double kMinRadiusSpan = 1.0;
inline constexpr double kMinSweepDeg = 0.1;

bool isValidSeam(std::string_view seam);
bool isValidScaleSource(std::string_view source);

// A ray arc's umbilicus, built once (immutable, shared by workers): the dense
// per-slice centers Umbilicus::FromPoints interpolates over the region frame's
// z extent, and the seam direction.
struct CompiledArc {
    std::vector<cv::Vec2d> centers;
    cv::Vec2d seam{1.0, 0.0};
    // The center Umbilicus::theta uses for height z: index clamp(lround(z)).
    cv::Vec2d centerAt(double z) const;
    // The unit XY direction at angle `thetaDeg` from the seam.
    cv::Vec2d direction(double thetaDeg) const;
};

std::optional<CompiledArc> compileArc(const RayArc& arc, const Frame& frame, std::string* error);
// XY distance to the umbilicus center of the point's slice (not the 3D
// distance_to_umbilicus), and the angle from the seam in [0, 360), computed in
// double from the same centers and seam Umbilicus::theta uses.
double radiusXY(const CompiledArc& compiled, const cv::Vec3d& regionPoint);
double thetaDeg(const CompiledArc& compiled, const cv::Vec3d& regionPoint);
bool inSector(const RayArc& arc, double thetaDeg);
bool contains(const RayArc& arc, const CompiledArc& compiled, const cv::Vec3d& regionPoint);

enum class Coverage { Outside, Partial, Inside };
// Conservative classification of a region-frame block [lo, hi]: Inside or
// Outside only when every point is, Partial when unsure.
Coverage classify(const RayArc& arc, const CompiledArc& compiled, const cv::Vec3d& lo, const cv::Vec3d& hi);
// Region-frame bounding box of the arc.
std::array<cv::Vec3d, 2> arcBounds(const RayArc& arc, const CompiledArc& compiled);

// Outline on a horizontal (AxisZ) plane at region height z: closed polylines
// in the region frame (two circles for a full sweep), at most `stepDeg`
// degrees apart.
std::vector<std::vector<cv::Vec3d>> arcOutlineHorizontal(const RayArc& arc, const CompiledArc& compiled, double zRegion, double stepDeg);
// The intervals of u where the horizontal line originXY + u * h (h a unit XY
// vector) at slice m is inside the arc.
std::vector<std::pair<double, double>> arcRowIntervals(const RayArc& arc, const CompiledArc& compiled, const cv::Vec2d& originXY, const cv::Vec2d& h, int slice);
// Outline on a vertical plane through originXY with horizontal direction h:
// the boundary of the per-slice rows (exact for rowStride 1), as segments in
// the region frame, for slices [sliceFrom, sliceTo].
std::vector<std::array<cv::Vec3d, 2>> arcOutlineVertical(
    const RayArc& arc, const CompiledArc& compiled, const cv::Vec2d& originXY, const cv::Vec2d& h, int sliceFrom, int sliceTo, int rowStride);

enum class ArcHandleKind { RadiusMin, RadiusMax, ThetaStart, ThetaEnd };
struct ArcHandle {
    ArcHandleKind kind = ArcHandleKind::RadiusMin;
    cv::Vec3d regionPos{0, 0, 0};
};
// Radius handles at mid-angle and angle handles at mid-radius, on a
// horizontal plane at region height z.
std::vector<ArcHandle> arcHandles(const RayArc& arc, const CompiledArc& compiled, double zRegion);
RayArc dragArcHandle(const RayArc& start, const CompiledArc& compiled, ArcHandleKind kind, const cv::Vec3d& cursorRegion);
// The arc from two clicks: angles and radii from the clicks, the angle running
// from the first to the second click (increasing) or back, z the slice
// +/- depth/2 clamped to [0, zMaxIndex]. None when the radii are under one
// voxel apart.
std::optional<RayArc> arcFromClicks(
    const CompiledArc& compiled, const cv::Vec3d& firstRegion, const cv::Vec3d& secondRegion, bool increasing, double sliceRegion, int depthVoxels, int zMaxIndex);

using Shape = std::variant<Box, RayArc>;

enum class Status { Open, Resolved };

struct Region {
    std::string id;
    std::string name;
    std::string color{"#ffb000"};
    std::string notes;
    std::string author;
    std::string created;
    std::string modified;
    std::vector<std::string> tags;
    Status status = Status::Open;
    std::uint64_t revision = 0;
    Frame frame;
    Shape shape = Box{};
};

inline const Box* boxOf(const Region& region)
{
    return std::get_if<Box>(&region.shape);
}
inline Box* boxOf(Region& region)
{
    return std::get_if<Box>(&region.shape);
}
inline const RayArc* arcOf(const Region& region)
{
    return std::get_if<RayArc>(&region.shape);
}
std::string_view shapeTypeName(const Shape& shape);

struct Loaded {
    Region region;
    bool readOnly = false;
    std::string readOnlyReason;
};

struct ParseResult {
    std::optional<Loaded> loaded;
    std::string error;
    std::string errorPath;
};

std::string_view statusName(Status status);
std::optional<Status> parseStatus(std::string_view text);

// True for ids that are safe as a file name stem: 1 to 64 of [A-Za-z0-9_-].
bool isValidId(std::string_view id);
bool isValidColor(std::string_view color);

// First problem with a region's fields, as (JSON path, message); nullopt when
// the region is valid. Used by parseRegion() and before every write.
std::optional<std::pair<std::string, std::string>> validateRegion(const Region& region);

// Parses one region file. A document newer than kVersion, or one with a key
// this version does not know, loads read-only with the reason set.
ParseResult parseRegion(const Json& document);
// The document for a region, keys in a fixed order, so a file written once
// round-trips byte for byte.
Json toJson(const Region& region);

// Annotation extent of a region frame, x y z order, and its largest index.
cv::Vec3i frameExtentXyz(const Frame& frame);
cv::Vec3i frameMaxIndexXyz(const Frame& frame);

struct FrameMap {
    int log2RegionToViewer = 0;
    double regionToViewer() const;
    double viewerToRegion() const;
};

struct FrameCheck {
    std::optional<FrameMap> map;
    std::string reason;
};

// Maps a region frame onto the volume whose annotation frame is `volumeFrame`
// and whose coordinate-space tag is `volumeCoordinateSpace`. Refused with a
// user-facing reason when the coordinate spaces conflict, no dyadic level fits
// the two extents, or both voxel sizes are known and disagree.
FrameCheck mapRegionFrame(const Frame& region, const vc3d::annotation::AnnotationFrame& volumeFrame, std::string_view volumeCoordinateSpace);

// The frame a region drawn on this volume records. Empty base shape when the
// volume's annotation frame is unknown.
Frame frameForVolume(const vc3d::annotation::AnnotationFrame& volumeFrame, std::string volumeId, const Json& coordinateIdentity);

// A slice plane in viewer coordinates.
struct Plane {
    cv::Vec3d origin{0, 0, 0};
    cv::Vec3d normal{0, 0, 1};
    cv::Vec3d basisX{1, 0, 0};
    cv::Vec3d basisY{0, 1, 0};
};

enum class PlaneKind { AxisX, AxisY, AxisZ, Vertical, Tilted };

PlaneKind classifyPlane(const Plane& plane);
// The two box axes a plane of this kind can edit, or {-1, -1}.
std::array<int, 2> inPlaneAxes(PlaneKind kind);

// The plane's cross-section of the face box [min - 0.5, max + 0.5] * k, in
// viewer coordinates, ordered by angle in the plane basis: empty or 3 to 6
// vertices.
std::vector<cv::Vec3d> boxCrossSection(const Box& box, const FrameMap& map, const Plane& plane);

enum class HandleKind { None, Corner, Edge, Body };

struct Handle {
    HandleKind kind = HandleKind::None;
    // Box axes the handle moves (-1 unused) and, per axis, which face: -1 the
    // min face, +1 the max face, 0 the whole box (Body).
    std::array<int, 2> axes{-1, -1};
    std::array<int, 2> sides{0, 0};
    cv::Vec3d viewerPos{0, 0, 0};
};

// Corner and edge handles and a Body handle at the center (it moves the whole
// box in the plane) on axis planes, z edges on vertical planes, none on tilted
// planes. A press elsewhere inside the outline is not a handle, so it keeps
// its meaning (selecting the region under it).
std::vector<Handle> boxHandles(const Box& box, const FrameMap& map, const Plane& plane, PlaneKind kind);

// The box after dragging `handle` from `pressViewer` to `cursorViewer`:
// integer, clamped to [0, extentMaxXyz], min and max swapped when a face is
// dragged past the opposite one.
Box dragHandle(const Box& start, const Handle& handle, const cv::Vec3d& pressViewer, const cv::Vec3d& cursorViewer, const FrameMap& map, const cv::Vec3i& extentMaxXyz);

// A new box from a drag on a plane: the two in-plane axes from the drag, the
// third `depthVoxels` (region frame) centered on the slice and clamped to the
// extent. None for a drag under one voxel, or on a vertical or tilted plane.
std::optional<Box> boxFromDrag(
    const cv::Vec3d& pressViewer,
    const cv::Vec3d& cursorViewer,
    PlaneKind kind,
    const cv::Vec3d& planeOriginViewer,
    int depthVoxels,
    const FrameMap& map,
    const cv::Vec3i& extentMaxXyz);

bool contains(const Box& box, const cv::Vec3d& regionPoint);
std::uint64_t voxelCount(const Box& box);
Rect3D focusBounds(const Box& box, const FrameMap& map);
cv::Vec3d centerViewer(const Box& box, const FrameMap& map);
cv::Vec3d toRegion(const cv::Vec3d& viewerPoint, const FrameMap& map);
cv::Vec3d toViewer(const cv::Vec3d& regionPoint, const FrameMap& map);

// Region tools take pointer input only in standard-role slice viewers.
bool shouldInstallTools(std::string_view role, std::string_view surfaceName);

// "x 4100..4356  y 3900..4156  z 6000..6128  (257 x 257 x 129 voxels)"
std::string describeBox(const Box& box);
// "theta 30..55 deg  r 900..1400  z 6000..6400"
std::string describeArc(const RayArc& arc);
std::string describeShape(const Shape& shape);

}  // namespace vc3d::regions
