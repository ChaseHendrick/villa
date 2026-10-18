#include "RegionModel.hpp"

#include "OpenDataCoordinateIdentity.hpp"

#include "vc/lasagna/Manifest.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace vc3d::regions
{

namespace
{

// The vc_open_data_* keys a region frame may carry, in the order written.
constexpr std::array<std::string_view, 5> kIdentityKeys = {
    "vc_open_data_coordinate_space",
    "vc_open_data_source_path",
    "vc_open_data_source_coordinate_level",
    "vc_open_data_source_coordinate_scale_factor",
    "vc_open_data_source_original_resolution",
};

constexpr std::array<std::string_view, 14> kTopLevelKeys = {
    "schema",
    "version",
    "id",
    "name",
    "color",
    "tags",
    "status",
    "notes",
    "author",
    "created",
    "modified",
    "revision",
    "frame",
    "shape",
};

constexpr const char* kAxisNames[3] = {"x", "y", "z"};

bool isKnownKey(std::string_view key, std::span<const std::string_view> known)
{
    return !key.empty() && std::find(known.begin(), known.end(), key) != known.end();
}

std::string formatNumber(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string formatShapeZyx(long long z, long long y, long long x)
{
    return std::to_string(z) + " x " + std::to_string(y) + " x " + std::to_string(x);
}

bool isPowerOfTwo(double value, int* exponent)
{
    if (!(value > 0.0) || !std::isfinite(value)) {
        return false;
    }
    int e = 0;
    const double mantissa = std::frexp(value, &e);
    if (mantissa != 0.5) {
        return false;
    }
    if (exponent) {
        *exponent = e - 1;
    }
    return true;
}

struct Parser {
    ParseResult result;
    std::string firstUnknown;

    bool fail(std::string path, std::string message)
    {
        if (result.error.empty()) {
            result.errorPath = std::move(path);
            result.error = std::move(message);
        }
        return false;
    }

    void noteUnknown(const Json& object, std::span<const std::string_view> known, const std::string& prefix)
    {
        if (!firstUnknown.empty() || !object.is_object()) {
            return;
        }
        for (const auto& item : object.items()) {
            if (!isKnownKey(item.key(), known)) {
                firstUnknown = prefix.empty() ? item.key() : prefix + "." + item.key();
                return;
            }
        }
    }

    bool optionalString(const Json& object, const char* key, const std::string& path, std::string& out)
    {
        if (!object.contains(key) || object[key].is_null()) {
            return true;
        }
        if (!object[key].is_string()) {
            return fail(path, "expected a string");
        }
        out = object[key].get<std::string>();
        return true;
    }

    bool integer(const Json& value, const std::string& path, long long& out)
    {
        if (!value.is_number_integer()) {
            return fail(path, "expected an integer");
        }
        if (value.is_number_unsigned()) {
            const auto u = value.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
                return fail(path, "integer out of range");
            }
            out = static_cast<long long>(u);
            return true;
        }
        out = value.get<long long>();
        return true;
    }

    bool point(const Json& object, const std::string& path, cv::Vec3i& out)
    {
        if (!object.is_object()) {
            return fail(path, "expected an object with x, y and z");
        }
        static constexpr std::array<std::string_view, 3> kKeys = {"x", "y", "z"};
        noteUnknown(object, kKeys, path);
        for (int axis = 0; axis < 3; ++axis) {
            const std::string axisPath = path + "." + kAxisNames[axis];
            if (!object.contains(kAxisNames[axis])) {
                return fail(axisPath, "missing");
            }
            long long value = 0;
            if (!integer(object[kAxisNames[axis]], axisPath, value)) {
                return false;
            }
            if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
                return fail(axisPath, "integer out of range");
            }
            out[axis] = static_cast<int>(value);
        }
        return true;
    }
};

}  // namespace

std::string_view statusName(Status status)
{
    return status == Status::Resolved ? "resolved" : "open";
}

std::optional<Status> parseStatus(std::string_view text)
{
    if (text == "open") {
        return Status::Open;
    }
    if (text == "resolved") {
        return Status::Resolved;
    }
    return std::nullopt;
}

bool isValidId(std::string_view id)
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '-' || ch == '_';
    });
}

bool isValidColor(std::string_view color)
{
    if (color.size() != 7 || color.front() != '#') {
        return false;
    }
    return std::all_of(color.begin() + 1, color.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
}

cv::Vec3i frameExtentXyz(const Frame& frame)
{
    const auto clampInt = [](long long v) { return static_cast<int>(std::clamp<long long>(v, 0, std::numeric_limits<int>::max())); };
    return {clampInt(frame.baseShapeZyx[2]), clampInt(frame.baseShapeZyx[1]), clampInt(frame.baseShapeZyx[0])};
}

cv::Vec3i frameMaxIndexXyz(const Frame& frame)
{
    const cv::Vec3i extent = frameExtentXyz(frame);
    return {extent[0] - 1, extent[1] - 1, extent[2] - 1};
}

std::optional<std::pair<std::string, std::string>> validateRegion(const Region& region)
{
    if (!isValidId(region.id)) {
        return std::pair<std::string, std::string>{"id", "expected 1 to 64 letters, digits, '-' or '_'"};
    }
    if (!isValidColor(region.color)) {
        return std::pair<std::string, std::string>{"color", "expected a color like \"#ffb000\""};
    }
    for (std::size_t i = 0; i < region.tags.size(); ++i) {
        if (region.tags[i].empty()) {
            return std::pair<std::string, std::string>{"tags[" + std::to_string(i) + "]", "expected a non-empty string"};
        }
    }
    if (region.revision < 1) {
        return std::pair<std::string, std::string>{"revision", "expected an integer of at least 1"};
    }
    for (int i = 0; i < 3; ++i) {
        if (region.frame.baseShapeZyx[i] <= 0 || region.frame.baseShapeZyx[i] > std::numeric_limits<int>::max()) {
            return std::pair<
                std::string,
                std::string>{"frame.coordinate_base_shape_zyx[" + std::to_string(i) + "]", "expected a positive voxel count"};
        }
    }
    if (region.frame.voxelSizeUm && (!std::isfinite(*region.frame.voxelSizeUm) || *region.frame.voxelSizeUm <= 0.0)) {
        return std::pair<std::string, std::string>{"frame.voxel_size_um", "expected a positive number"};
    }
    const cv::Vec3i maxIndex = frameMaxIndexXyz(region.frame);
    for (int axis = 0; axis < 3; ++axis) {
        const std::string a = kAxisNames[axis];
        if (region.shape.min[axis] < 0) {
            return std::pair<std::string, std::string>{"shape.min." + a, "outside the volume (below 0)"};
        }
        if (region.shape.max[axis] < region.shape.min[axis]) {
            return std::pair<std::string, std::string>{"shape.max." + a, "smaller than shape.min." + a};
        }
        if (region.shape.max[axis] > maxIndex[axis]) {
            return std::pair<
                std::string,
                std::string>{"shape.max." + a, "outside the volume (largest index " + std::to_string(maxIndex[axis]) + ")"};
        }
    }
    return std::nullopt;
}

ParseResult parseRegion(const Json& document)
{
    Parser p;
    Region region;
    long long version = 0;

    const auto finish = [&]() -> ParseResult { return p.result; };

    if (!document.is_object()) {
        p.fail("", "expected a JSON object");
        return finish();
    }
    p.noteUnknown(document, kTopLevelKeys, "");

    if (!document.contains("schema")) {
        p.fail("schema", "missing");
        return finish();
    }
    if (!document["schema"].is_string() || document["schema"].get<std::string>() != kSchema) {
        p.fail("schema", "expected \"vc3d.region\"");
        return finish();
    }
    if (!document.contains("version")) {
        p.fail("version", "missing");
        return finish();
    }
    if (!p.integer(document["version"], "version", version)) {
        return finish();
    }
    if (version < 1) {
        p.fail("version", "expected 1 or newer");
        return finish();
    }

    if (!document.contains("id")) {
        p.fail("id", "missing");
        return finish();
    }
    if (!document["id"].is_string()) {
        p.fail("id", "expected a string");
        return finish();
    }
    region.id = document["id"].get<std::string>();
    if (!p.optionalString(document, "name", "name", region.name) || !p.optionalString(document, "color", "color", region.color) ||
        !p.optionalString(document, "notes", "notes", region.notes) || !p.optionalString(document, "author", "author", region.author) ||
        !p.optionalString(document, "created", "created", region.created) || !p.optionalString(document, "modified", "modified", region.modified)) {
        return finish();
    }
    if (document.contains("tags") && !document["tags"].is_null()) {
        const Json& tags = document["tags"];
        if (!tags.is_array()) {
            p.fail("tags", "expected an array of strings");
            return finish();
        }
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (!tags[i].is_string()) {
                p.fail("tags[" + std::to_string(i) + "]", "expected a string");
                return finish();
            }
            region.tags.push_back(tags[i].get<std::string>());
        }
    }
    if (document.contains("status") && !document["status"].is_null()) {
        if (!document["status"].is_string()) {
            p.fail("status", "expected \"open\" or \"resolved\"");
            return finish();
        }
        const auto status = parseStatus(document["status"].get<std::string>());
        if (!status) {
            p.fail("status", "expected \"open\" or \"resolved\"");
            return finish();
        }
        region.status = *status;
    }
    if (!document.contains("revision")) {
        p.fail("revision", "missing");
        return finish();
    }
    long long revision = 0;
    if (!p.integer(document["revision"], "revision", revision)) {
        return finish();
    }
    if (revision < 1) {
        p.fail("revision", "expected an integer of at least 1");
        return finish();
    }
    region.revision = static_cast<std::uint64_t>(revision);

    if (!document.contains("frame")) {
        p.fail("frame", "missing");
        return finish();
    }
    const Json& frame = document["frame"];
    if (!frame.is_object()) {
        p.fail("frame", "expected an object");
        return finish();
    }
    {
        std::vector<std::string_view> known = {"coordinate_base_shape_zyx", "voxel_size_um", "volume_id"};
        known.insert(known.end(), kIdentityKeys.begin(), kIdentityKeys.end());
        p.noteUnknown(frame, known, "frame");
    }
    if (!frame.contains("coordinate_base_shape_zyx")) {
        p.fail("frame.coordinate_base_shape_zyx", "missing");
        return finish();
    }
    const Json& shapeZyx = frame["coordinate_base_shape_zyx"];
    if (!shapeZyx.is_array() || shapeZyx.size() != 3) {
        p.fail("frame.coordinate_base_shape_zyx", "expected three voxel counts, z y x");
        return finish();
    }
    for (std::size_t i = 0; i < 3; ++i) {
        long long value = 0;
        if (!p.integer(shapeZyx[i], "frame.coordinate_base_shape_zyx[" + std::to_string(i) + "]", value)) {
            return finish();
        }
        region.frame.baseShapeZyx[i] = value;
    }
    if (frame.contains("voxel_size_um") && !frame["voxel_size_um"].is_null()) {
        if (!frame["voxel_size_um"].is_number()) {
            p.fail("frame.voxel_size_um", "expected a positive number");
            return finish();
        }
        region.frame.voxelSizeUm = frame["voxel_size_um"].get<double>();
    }
    if (!p.optionalString(frame, "volume_id", "frame.volume_id", region.frame.volumeId)) {
        return finish();
    }
    for (const auto key : kIdentityKeys) {
        const std::string k(key);
        if (frame.contains(k)) {
            region.frame.coordinateIdentity[k] = frame[k];
        }
    }
    if (frame.contains("vc_open_data_coordinate_space") && !frame["vc_open_data_coordinate_space"].is_null()) {
        if (!frame["vc_open_data_coordinate_space"].is_string()) {
            p.fail("frame.vc_open_data_coordinate_space", "expected a string");
            return finish();
        }
        region.frame.coordinateSpace = frame["vc_open_data_coordinate_space"].get<std::string>();
    }

    if (!document.contains("shape")) {
        p.fail("shape", "missing");
        return finish();
    }
    const Json& shape = document["shape"];
    if (!shape.is_object()) {
        p.fail("shape", "expected an object");
        return finish();
    }
    if (!shape.contains("type") || !shape["type"].is_string()) {
        p.fail("shape.type", "expected a string");
        return finish();
    }
    const std::string type = shape["type"].get<std::string>();
    if (type != "box") {
        // A newer VC3D may add shape types; say so instead of calling the file broken.
        p.fail(
            "shape.type",
            version > kVersion ? "shape type \"" + type + "\" was written by a newer VC3D (region version " + std::to_string(version) + ")"
                               : "unknown shape type \"" + type + "\"");
        return finish();
    }
    static constexpr std::array<std::string_view, 3> kBoxKeys = {"type", "min", "max"};
    p.noteUnknown(shape, kBoxKeys, "shape");
    if (!shape.contains("min")) {
        p.fail("shape.min", "missing");
        return finish();
    }
    if (!shape.contains("max")) {
        p.fail("shape.max", "missing");
        return finish();
    }
    if (!p.point(shape["min"], "shape.min", region.shape.min) || !p.point(shape["max"], "shape.max", region.shape.max)) {
        return finish();
    }

    if (const auto problem = validateRegion(region)) {
        p.fail(problem->first, problem->second);
        return finish();
    }

    Loaded loaded;
    loaded.region = std::move(region);
    if (version > kVersion) {
        loaded.readOnly = true;
        loaded.readOnlyReason = "written by a newer VC3D (region version " + std::to_string(version) + ")";
    } else if (!p.firstUnknown.empty()) {
        loaded.readOnly = true;
        loaded.readOnlyReason = "unknown field `" + p.firstUnknown + "`";
    }
    p.result.loaded = std::move(loaded);
    return finish();
}

Json toJson(const Region& region)
{
    Json doc = Json::object();
    doc["schema"] = std::string(kSchema);
    doc["version"] = kVersion;
    doc["id"] = region.id;
    doc["name"] = region.name;
    doc["color"] = region.color;
    doc["tags"] = Json::array();
    for (const auto& tag : region.tags) {
        doc["tags"].push_back(tag);
    }
    doc["status"] = std::string(statusName(region.status));
    doc["notes"] = region.notes;
    doc["author"] = region.author;
    doc["created"] = region.created;
    doc["modified"] = region.modified;
    doc["revision"] = region.revision;

    Json frame = Json::object();
    frame["coordinate_base_shape_zyx"] = Json::array({region.frame.baseShapeZyx[0], region.frame.baseShapeZyx[1], region.frame.baseShapeZyx[2]});
    if (region.frame.voxelSizeUm) {
        frame["voxel_size_um"] = *region.frame.voxelSizeUm;
    }
    frame["volume_id"] = region.frame.volumeId;
    for (const auto key : kIdentityKeys) {
        const std::string k(key);
        if (region.frame.coordinateIdentity.is_object() && region.frame.coordinateIdentity.contains(k)) {
            frame[k] = region.frame.coordinateIdentity[k];
        }
    }
    doc["frame"] = std::move(frame);

    const auto point = [](const cv::Vec3i& p) {
        Json o = Json::object();
        o["x"] = p[0];
        o["y"] = p[1];
        o["z"] = p[2];
        return o;
    };
    Json shape = Json::object();
    shape["type"] = "box";
    shape["min"] = point(region.shape.min);
    shape["max"] = point(region.shape.max);
    doc["shape"] = std::move(shape);
    return doc;
}

double FrameMap::regionToViewer() const
{
    return std::ldexp(1.0, log2RegionToViewer);
}

double FrameMap::viewerToRegion() const
{
    return std::ldexp(1.0, -log2RegionToViewer);
}

FrameCheck mapRegionFrame(const Frame& region, const vc3d::annotation::AnnotationFrame& volumeFrame, std::string_view volumeCoordinateSpace)
{
    FrameCheck check;
    if (!vc3d::opendata::coordinateSpaceTagsCompatible(region.coordinateSpace, volumeCoordinateSpace)) {
        check.reason = "Drawn on another scan (coordinate space " + region.coordinateSpace + ", this volume " +
                       std::string(volumeCoordinateSpace) + ")";
        return check;
    }
    std::array<long long, 3> volumeZyx{};
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = volumeFrame.extentXyz[2 - axis];
        if (!std::isfinite(extent) || extent < 1.0) {
            check.reason = "This volume's annotation frame is unknown (no voxel counts)";
            return check;
        }
        volumeZyx[axis] = std::llround(extent);
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (region.baseShapeZyx[axis] <= 0) {
            check.reason = "The region has no grid";
            return check;
        }
    }
    int log2Factor = 0;
    if (!isPowerOfTwo(volumeFrame.factor, &log2Factor)) {
        check.reason = "This volume's coordinate factor " + formatNumber(volumeFrame.factor) + " is not a power of two";
        return check;
    }
    // The grids are compared at the volume's own level: its annotation extent
    // is its voxel counts times the factor, which rounds a rebased store up
    // (a 20974-voxel source stored as 5244 voxels at level 2 has extent 20976).
    std::array<std::size_t, 3> nativeZyx{};
    for (int axis = 0; axis < 3; ++axis) {
        nativeZyx[axis] = static_cast<std::size_t>(std::max(1LL, std::llround(static_cast<double>(volumeZyx[axis]) / volumeFrame.factor)));
    }
    double regionToAnnotation = 0.0;
    try {
        regionToAnnotation = volumeFrame.factor * vc::lasagna::dyadicCoordinateScaleBetweenShapes(
                                                      {static_cast<std::size_t>(region.baseShapeZyx[0]),
                                                       static_cast<std::size_t>(region.baseShapeZyx[1]),
                                                       static_cast<std::size_t>(region.baseShapeZyx[2])},
                                                      nativeZyx,
                                                      5);
    } catch (const std::exception&) {
        check.reason = "Grid does not match this volume (" +
                       formatShapeZyx(region.baseShapeZyx[0], region.baseShapeZyx[1], region.baseShapeZyx[2]) + " vs " +
                       formatShapeZyx(volumeZyx[0], volumeZyx[1], volumeZyx[2]) + ")";
        return check;
    }
    if (region.voxelSizeUm && volumeFrame.voxelSizeUm) {
        const double regionVoxel = *region.voxelSizeUm;
        const double volumeVoxel = regionToAnnotation * *volumeFrame.voxelSizeUm;
        if (std::abs(regionVoxel - volumeVoxel) > 1e-6 * std::max(regionVoxel, volumeVoxel)) {
            check.reason = "Voxel size differs (" + formatNumber(regionVoxel) + " vs " + formatNumber(volumeVoxel) + " um)";
            return check;
        }
    }
    int log2RegionToAnnotation = 0;
    if (!isPowerOfTwo(regionToAnnotation, &log2RegionToAnnotation)) {
        check.reason = "Grid does not match this volume (not a power-of-two scale)";
        return check;
    }
    check.map = FrameMap{log2RegionToAnnotation - log2Factor};
    return check;
}

Frame frameForVolume(const vc3d::annotation::AnnotationFrame& volumeFrame, std::string volumeId, const Json& coordinateIdentity)
{
    Frame frame;
    bool known = true;
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = volumeFrame.extentXyz[axis];
        if (!std::isfinite(extent) || extent < 1.0) {
            known = false;
        }
    }
    if (known) {
        frame.baseShapeZyx = {std::llround(volumeFrame.extentXyz[2]), std::llround(volumeFrame.extentXyz[1]), std::llround(volumeFrame.extentXyz[0])};
    }
    frame.voxelSizeUm = volumeFrame.voxelSizeUm;
    frame.volumeId = std::move(volumeId);
    frame.coordinateIdentity = Json::object();
    if (coordinateIdentity.is_object()) {
        for (const auto key : kIdentityKeys) {
            const std::string k(key);
            if (coordinateIdentity.contains(k)) {
                frame.coordinateIdentity[k] = coordinateIdentity[k];
            }
        }
        if (coordinateIdentity.contains("vc_open_data_coordinate_space") && coordinateIdentity["vc_open_data_coordinate_space"].is_string()) {
            frame.coordinateSpace = coordinateIdentity["vc_open_data_coordinate_space"].get<std::string>();
        }
    }
    return frame;
}

PlaneKind classifyPlane(const Plane& plane)
{
    const double length = cv::norm(plane.normal);
    if (!(length > 0.0) || !std::isfinite(length)) {
        return PlaneKind::Tilted;
    }
    const cv::Vec3d n = plane.normal / length;
    constexpr double kAxisTolerance = 1e-6;
    if (std::abs(n[2]) > 1.0 - kAxisTolerance) {
        return PlaneKind::AxisZ;
    }
    if (std::abs(n[0]) > 1.0 - kAxisTolerance) {
        return PlaneKind::AxisX;
    }
    if (std::abs(n[1]) > 1.0 - kAxisTolerance) {
        return PlaneKind::AxisY;
    }
    if (std::abs(n[2]) < kAxisTolerance) {
        return PlaneKind::Vertical;
    }
    return PlaneKind::Tilted;
}

std::array<int, 2> inPlaneAxes(PlaneKind kind)
{
    switch (kind) {
        case PlaneKind::AxisZ:
            return {0, 1};
        case PlaneKind::AxisY:
            return {0, 2};
        case PlaneKind::AxisX:
            return {1, 2};
        default:
            return {-1, -1};
    }
}

namespace
{

struct FaceBox {
    cv::Vec3d lo;
    cv::Vec3d hi;
};

FaceBox faceBox(const Box& box, const FrameMap& map)
{
    const double k = map.regionToViewer();
    FaceBox face;
    for (int axis = 0; axis < 3; ++axis) {
        face.lo[axis] = (static_cast<double>(box.min[axis]) - 0.5) * k;
        face.hi[axis] = (static_cast<double>(box.max[axis]) + 0.5) * k;
    }
    return face;
}

cv::Vec3d unitNormal(const Plane& plane)
{
    const double length = cv::norm(plane.normal);
    return length > 0.0 ? cv::Vec3d(plane.normal / length) : cv::Vec3d(0, 0, 1);
}

}  // namespace

std::vector<cv::Vec3d> boxCrossSection(const Box& box, const FrameMap& map, const Plane& plane)
{
    const FaceBox face = faceBox(box, map);
    const cv::Vec3d n = unitNormal(plane);
    std::array<cv::Vec3d, 8> corners;
    std::array<double, 8> signedDistance;
    for (int i = 0; i < 8; ++i) {
        corners[i] = {
            (i & 1) ? face.hi[0] : face.lo[0],
            (i & 2) ? face.hi[1] : face.lo[1],
            (i & 4) ? face.hi[2] : face.lo[2],
        };
        signedDistance[i] = (corners[i] - plane.origin).dot(n);
    }

    std::vector<cv::Vec3d> points;
    const auto addPoint = [&points](const cv::Vec3d& p) {
        for (const auto& q : points) {
            if (cv::norm(p - q) <= 1e-9) {
                return;
            }
        }
        points.push_back(p);
    };
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (i & bit) {
                continue;
            }
            const int j = i | bit;
            const double s0 = signedDistance[i];
            const double s1 = signedDistance[j];
            if (s0 == 0.0) {
                addPoint(corners[i]);
            }
            if (s1 == 0.0) {
                addPoint(corners[j]);
            }
            if ((s0 < 0.0 && s1 > 0.0) || (s0 > 0.0 && s1 < 0.0)) {
                const double t = s0 / (s0 - s1);
                addPoint(corners[i] + (corners[j] - corners[i]) * t);
            }
        }
    }
    if (points.size() < 3) {
        return {};
    }

    cv::Vec3d centroid(0, 0, 0);
    for (const auto& p : points) {
        centroid += p;
    }
    centroid *= 1.0 / static_cast<double>(points.size());
    std::vector<std::pair<double, cv::Vec3d>> ordered;
    ordered.reserve(points.size());
    for (const auto& p : points) {
        const cv::Vec3d d = p - centroid;
        ordered.emplace_back(std::atan2(d.dot(plane.basisY), d.dot(plane.basisX)), p);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<cv::Vec3d> polygon;
    polygon.reserve(ordered.size());
    for (const auto& [angle, p] : ordered) {
        polygon.push_back(p);
    }
    return polygon;
}

std::vector<Handle> boxHandles(const Box& box, const FrameMap& map, const Plane& plane, PlaneKind kind)
{
    std::vector<Handle> handles;
    const std::vector<cv::Vec3d> section = boxCrossSection(box, map, plane);
    if (section.empty()) {
        return handles;
    }
    const FaceBox face = faceBox(box, map);
    const cv::Vec3d n = unitNormal(plane);

    if (kind == PlaneKind::AxisX || kind == PlaneKind::AxisY || kind == PlaneKind::AxisZ) {
        const auto axes = inPlaneAxes(kind);
        const int a = axes[0];
        const int b = axes[1];
        const int c = 3 - a - b;
        const auto onPlane = [&](double va, double vb) {
            cv::Vec3d p(0, 0, 0);
            p[a] = va;
            p[b] = vb;
            p[c] = plane.origin[c] - (n[a] * (va - plane.origin[a]) + n[b] * (vb - plane.origin[b])) / n[c];
            return p;
        };
        const auto faceAt = [&](int axis, int side) { return side < 0 ? face.lo[axis] : face.hi[axis]; };
        for (int sa : {-1, 1}) {
            for (int sb : {-1, 1}) {
                handles.push_back({HandleKind::Corner, {a, b}, {sa, sb}, onPlane(faceAt(a, sa), faceAt(b, sb))});
            }
        }
        const double midA = 0.5 * (face.lo[a] + face.hi[a]);
        const double midB = 0.5 * (face.lo[b] + face.hi[b]);
        for (int s : {-1, 1}) {
            handles.push_back({HandleKind::Edge, {a, -1}, {s, 0}, onPlane(faceAt(a, s), midB)});
            handles.push_back({HandleKind::Edge, {b, -1}, {s, 0}, onPlane(midA, faceAt(b, s))});
        }
        handles.push_back({HandleKind::Body, {a, b}, {0, 0}, onPlane(midA, midB)});
        return handles;
    }

    if (kind == PlaneKind::Vertical) {
        const double scale = std::max({1.0, std::abs(face.lo[2]), std::abs(face.hi[2])});
        for (int side : {-1, 1}) {
            const double z = side < 0 ? face.lo[2] : face.hi[2];
            cv::Vec3d sum(0, 0, 0);
            int count = 0;
            for (const auto& p : section) {
                if (std::abs(p[2] - z) <= 1e-9 * scale) {
                    sum += p;
                    ++count;
                }
            }
            if (count >= 2) {
                handles.push_back({HandleKind::Edge, {2, -1}, {side, 0}, sum * (1.0 / count)});
            }
        }
    }
    return handles;
}

Box dragHandle(const Box& start, const Handle& handle, const cv::Vec3d& pressViewer, const cv::Vec3d& cursorViewer, const FrameMap& map, const cv::Vec3i& extentMaxXyz)
{
    Box box = start;
    const cv::Vec3d delta = (cursorViewer - pressViewer) * map.viewerToRegion();
    for (int i = 0; i < 2; ++i) {
        const int axis = handle.axes[i];
        if (axis < 0 || axis > 2 || !std::isfinite(delta[axis])) {
            continue;
        }
        const long long shift = std::llround(delta[axis]);
        const long long lo = start.min[axis];
        const long long hi = start.max[axis];
        const long long limit = std::max(0, extentMaxXyz[axis]);
        if (handle.kind == HandleKind::Body) {
            const long long clamped = std::clamp(shift, -lo, std::max(-lo, limit - hi));
            box.min[axis] = static_cast<int>(lo + clamped);
            box.max[axis] = static_cast<int>(hi + clamped);
            continue;
        }
        const int side = handle.sides[i];
        long long a = lo;
        long long b = hi;
        if (side > 0) {
            b = hi + shift;
        } else if (side < 0) {
            a = lo + shift;
        } else {
            continue;
        }
        const long long newMin = std::clamp(std::min(a, b), 0LL, limit);
        const long long newMax = std::clamp(std::max(a, b), 0LL, limit);
        box.min[axis] = static_cast<int>(newMin);
        box.max[axis] = static_cast<int>(newMax);
    }
    return box;
}

std::optional<Box> boxFromDrag(
    const cv::Vec3d& pressViewer,
    const cv::Vec3d& cursorViewer,
    PlaneKind kind,
    const cv::Vec3d& planeOriginViewer,
    int depthVoxels,
    const FrameMap& map,
    const cv::Vec3i& extentMaxXyz)
{
    const auto axes = inPlaneAxes(kind);
    if (axes[0] < 0) {
        return std::nullopt;
    }
    const cv::Vec3d a = toRegion(pressViewer, map);
    const cv::Vec3d b = toRegion(cursorViewer, map);
    Box box;
    for (const int axis : axes) {
        if (!std::isfinite(a[axis]) || !std::isfinite(b[axis]) || std::abs(b[axis] - a[axis]) < 1.0) {
            return std::nullopt;
        }
        const long long lo = std::max<long long>(0, std::llround(std::min(a[axis], b[axis])));
        const long long hi = std::min<long long>(extentMaxXyz[axis], std::llround(std::max(a[axis], b[axis])));
        if (lo > hi) {
            return std::nullopt;
        }
        box.min[axis] = static_cast<int>(lo);
        box.max[axis] = static_cast<int>(hi);
    }
    const int c = 3 - axes[0] - axes[1];
    const double slice = toRegion(planeOriginViewer, map)[c];
    if (!std::isfinite(slice)) {
        return std::nullopt;
    }
    const long long depth = std::max(1, depthVoxels);
    const long long lo = std::llround(slice) - depth / 2;
    const long long hi = lo + depth - 1;
    const long long clampedLo = std::max<long long>(0, lo);
    const long long clampedHi = std::min<long long>(extentMaxXyz[c], hi);
    if (clampedLo > clampedHi) {
        return std::nullopt;
    }
    box.min[c] = static_cast<int>(clampedLo);
    box.max[c] = static_cast<int>(clampedHi);
    return box;
}

bool contains(const Box& box, const cv::Vec3d& regionPoint)
{
    for (int axis = 0; axis < 3; ++axis) {
        if (!(regionPoint[axis] >= box.min[axis] && regionPoint[axis] <= box.max[axis])) {
            return false;
        }
    }
    return true;
}

std::uint64_t voxelCount(const Box& box)
{
    std::uint64_t count = 1;
    for (int axis = 0; axis < 3; ++axis) {
        if (box.max[axis] < box.min[axis]) {
            return 0;
        }
        count *= static_cast<std::uint64_t>(box.max[axis] - box.min[axis]) + 1;
    }
    return count;
}

Rect3D focusBounds(const Box& box, const FrameMap& map)
{
    const double k = map.regionToViewer();
    Rect3D rect;
    for (int axis = 0; axis < 3; ++axis) {
        rect.low[axis] = static_cast<float>(box.min[axis] * k);
        rect.high[axis] = static_cast<float>(box.max[axis] * k);
    }
    return rect;
}

cv::Vec3d centerViewer(const Box& box, const FrameMap& map)
{
    const double k = map.regionToViewer();
    cv::Vec3d center;
    for (int axis = 0; axis < 3; ++axis) {
        center[axis] = 0.5 * (static_cast<double>(box.min[axis]) + static_cast<double>(box.max[axis])) * k;
    }
    return center;
}

cv::Vec3d toRegion(const cv::Vec3d& viewerPoint, const FrameMap& map)
{
    return viewerPoint * map.viewerToRegion();
}

cv::Vec3d toViewer(const cv::Vec3d& regionPoint, const FrameMap& map)
{
    return regionPoint * map.regionToViewer();
}

bool shouldInstallTools(std::string_view role, std::string_view surfaceName)
{
    return role == "standard" && (surfaceName == "xy plane" || surfaceName == "seg xz" || surfaceName == "seg yz");
}

std::string describeBox(const Box& box)
{
    std::string text;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis > 0) {
            text += "  ";
        }
        text += std::string(kAxisNames[axis]) + " " + std::to_string(box.min[axis]) + ".." + std::to_string(box.max[axis]);
    }
    text += "  (" + std::to_string(box.max[0] - box.min[0] + 1) + " x " + std::to_string(box.max[1] - box.min[1] + 1) + " x " +
            std::to_string(box.max[2] - box.min[2] + 1) + " voxels)";
    return text;
}

}  // namespace vc3d::regions
