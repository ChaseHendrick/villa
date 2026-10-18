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
    Box shape;
};

inline const Box* boxOf(const Region& region)
{
    return &region.shape;
}
inline Box* boxOf(Region& region)
{
    return &region.shape;
}

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

}  // namespace vc3d::regions
