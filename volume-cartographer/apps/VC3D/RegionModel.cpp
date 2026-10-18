#include "RegionModel.hpp"

#include "OpenDataCoordinateIdentity.hpp"

#include "vc/core/util/Umbilicus.hpp"
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

    bool number(const Json& object, const char* key, const std::string& path, double& out)
    {
        if (!object.contains(key)) {
            return fail(path, "missing");
        }
        if (!object[key].is_number()) {
            return fail(path, "expected a number");
        }
        out = object[key].get<double>();
        return true;
    }

    bool pointD(const Json& object, const std::string& path, cv::Vec3d& out)
    {
        if (!object.is_object()) {
            return fail(path, "expected an object with x, y and z");
        }
        static constexpr std::array<std::string_view, 3> kKeys = {"x", "y", "z"};
        noteUnknown(object, kKeys, path);
        for (int axis = 0; axis < 3; ++axis) {
            if (!number(object, kAxisNames[axis], path + "." + kAxisNames[axis], out[axis])) {
                return false;
            }
        }
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
    if (const Box* box = boxOf(region)) {
        for (int axis = 0; axis < 3; ++axis) {
            const std::string a = kAxisNames[axis];
            if (box->min[axis] < 0) {
                return std::pair<std::string, std::string>{"shape.min." + a, "outside the volume (below 0)"};
            }
            if (box->max[axis] < box->min[axis]) {
                return std::pair<std::string, std::string>{"shape.max." + a, "smaller than shape.min." + a};
            }
            if (box->max[axis] > maxIndex[axis]) {
                return std::pair<
                    std::string,
                    std::string>{"shape.max." + a, "outside the volume (largest index " + std::to_string(maxIndex[axis]) + ")"};
            }
        }
    }
    if (const RayArc* arc = arcOf(region)) {
        using P = std::pair<std::string, std::string>;
        if (!std::isfinite(arc->thetaStartDeg) || arc->thetaStartDeg < 0.0 || arc->thetaStartDeg >= 360.0) {
            return P{"shape.theta_start_deg", "expected a number in [0, 360)"};
        }
        if (!std::isfinite(arc->thetaSweepDeg) || arc->thetaSweepDeg < kMinSweepDeg || arc->thetaSweepDeg > 360.0) {
            return P{"shape.theta_sweep_deg", "expected a number in [0.1, 360]"};
        }
        if (!std::isfinite(arc->radiusMin) || arc->radiusMin < 0.0) {
            return P{"shape.radius_min", "expected a number of at least 0"};
        }
        if (!std::isfinite(arc->radiusMax) || arc->radiusMax - arc->radiusMin < kMinRadiusSpan) {
            return P{"shape.radius_max", "expected at least radius_min + 1"};
        }
        if (arc->zMin < 0) {
            return P{"shape.z_min", "outside the volume (below 0)"};
        }
        if (arc->zMax < arc->zMin) {
            return P{"shape.z_max", "smaller than shape.z_min"};
        }
        if (arc->zMax > maxIndex[2]) {
            return P{"shape.z_max", "outside the volume (largest index " + std::to_string(maxIndex[2]) + ")"};
        }
        if (!isValidSeam(arc->umbilicus.seam)) {
            return P{"shape.umbilicus.seam", "expected \"+x\", \"-x\", \"+y\" or \"-y\""};
        }
        if (!isValidScaleSource(arc->umbilicus.scaleSource)) {
            return P{"shape.umbilicus.scale_source", "expected stamped_dimensions, stamped_voxel_size or inferred_from_grid"};
        }
        if (!std::isfinite(arc->umbilicus.scale) || arc->umbilicus.scale <= 0.0) {
            return P{"shape.umbilicus.scale", "expected a positive number"};
        }
        if (arc->umbilicus.controlPoints.empty()) {
            return P{"shape.umbilicus.control_points", "expected at least one point"};
        }
        for (std::size_t i = 0; i < arc->umbilicus.controlPoints.size(); ++i) {
            const cv::Vec3d& c = arc->umbilicus.controlPoints[i];
            if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
                return P{"shape.umbilicus.control_points[" + std::to_string(i) + "]", "expected finite numbers"};
            }
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
    if (type == "box") {
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
        Box box;
        if (!p.point(shape["min"], "shape.min", box.min) || !p.point(shape["max"], "shape.max", box.max)) {
            return finish();
        }
        region.shape = box;
    } else if (type == "ray_arc") {
        static constexpr std::array<std::string_view, 8> kArcKeys =
            {"type", "theta_start_deg", "theta_sweep_deg", "radius_min", "radius_max", "z_min", "z_max", "umbilicus"};
        p.noteUnknown(shape, kArcKeys, "shape");
        RayArc arc;
        long long zMin = 0;
        long long zMax = 0;
        if (!p.number(shape, "theta_start_deg", "shape.theta_start_deg", arc.thetaStartDeg) ||
            !p.number(shape, "theta_sweep_deg", "shape.theta_sweep_deg", arc.thetaSweepDeg) ||
            !p.number(shape, "radius_min", "shape.radius_min", arc.radiusMin) || !p.number(shape, "radius_max", "shape.radius_max", arc.radiusMax)) {
            return finish();
        }
        for (const auto& [key, out] : {std::pair<const char*, long long*>{"z_min", &zMin}, {"z_max", &zMax}}) {
            const std::string path = std::string("shape.") + key;
            if (!shape.contains(key)) {
                p.fail(path, "missing");
                return finish();
            }
            if (!p.integer(shape[key], path, *out)) {
                return finish();
            }
            if (*out < std::numeric_limits<int>::min() || *out > std::numeric_limits<int>::max()) {
                p.fail(path, "integer out of range");
                return finish();
            }
        }
        arc.zMin = static_cast<int>(zMin);
        arc.zMax = static_cast<int>(zMax);
        if (!shape.contains("umbilicus") || !shape["umbilicus"].is_object()) {
            p.fail("shape.umbilicus", shape.contains("umbilicus") ? "expected an object" : "missing");
            return finish();
        }
        const Json& u = shape["umbilicus"];
        static constexpr std::array<std::string_view, 5> kUmbilicusKeys = {"source_path", "scale", "scale_source", "seam", "control_points"};
        p.noteUnknown(u, kUmbilicusKeys, "shape.umbilicus");
        if (!p.optionalString(u, "source_path", "shape.umbilicus.source_path", arc.umbilicus.sourcePath) ||
            !p.number(u, "scale", "shape.umbilicus.scale", arc.umbilicus.scale) ||
            !p.optionalString(u, "scale_source", "shape.umbilicus.scale_source", arc.umbilicus.scaleSource) ||
            !p.optionalString(u, "seam", "shape.umbilicus.seam", arc.umbilicus.seam)) {
            return finish();
        }
        if (!u.contains("control_points") || !u["control_points"].is_array()) {
            p.fail("shape.umbilicus.control_points", "expected an array of points");
            return finish();
        }
        const Json& points = u["control_points"];
        for (std::size_t i = 0; i < points.size(); ++i) {
            cv::Vec3d point;
            if (!p.pointD(points[i], "shape.umbilicus.control_points[" + std::to_string(i) + "]", point)) {
                return finish();
            }
            arc.umbilicus.controlPoints.push_back(point);
        }
        region.shape = std::move(arc);
    } else {
        // A newer VC3D may add shape types; say so instead of calling the file broken.
        p.fail(
            "shape.type",
            version > kVersion ? "shape type \"" + type + "\" was written by a newer VC3D (region version " + std::to_string(version) + ")"
                               : "unknown shape type \"" + type + "\"");
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

    const auto point = [](const auto& p) {
        Json o = Json::object();
        o["x"] = p[0];
        o["y"] = p[1];
        o["z"] = p[2];
        return o;
    };
    Json shape = Json::object();
    if (const Box* box = boxOf(region)) {
        shape["type"] = "box";
        shape["min"] = point(box->min);
        shape["max"] = point(box->max);
    } else if (const RayArc* arc = arcOf(region)) {
        shape["type"] = "ray_arc";
        shape["theta_start_deg"] = arc->thetaStartDeg;
        shape["theta_sweep_deg"] = arc->thetaSweepDeg;
        shape["radius_min"] = arc->radiusMin;
        shape["radius_max"] = arc->radiusMax;
        shape["z_min"] = arc->zMin;
        shape["z_max"] = arc->zMax;
        Json u = Json::object();
        u["source_path"] = arc->umbilicus.sourcePath;
        u["scale"] = arc->umbilicus.scale;
        u["scale_source"] = arc->umbilicus.scaleSource;
        u["seam"] = arc->umbilicus.seam;
        u["control_points"] = Json::array();
        for (const auto& c : arc->umbilicus.controlPoints) {
            u["control_points"].push_back(point(c));
        }
        shape["umbilicus"] = std::move(u);
    }
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

std::string_view shapeTypeName(const Shape& shape)
{
    return std::holds_alternative<RayArc>(shape) ? "ray_arc" : "box";
}

std::string describeArc(const RayArc& arc)
{
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "theta %.2f..%.2f deg  r %.1f..%.1f  z %d..%d", arc.thetaStartDeg, arc.thetaStartDeg + arc.thetaSweepDeg, arc.radiusMin, arc.radiusMax, arc.zMin, arc.zMax);
    return buffer;
}

std::string describeShape(const Shape& shape)
{
    if (const auto* arc = std::get_if<RayArc>(&shape)) {
        return describeArc(*arc);
    }
    return describeBox(std::get<Box>(shape));
}

// ---------------------------------------------------------------------------
// Ray arcs
// ---------------------------------------------------------------------------

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

cv::Vec2d seamVector(std::string_view seam)
{
    if (seam == "-x") {
        return {-1.0, 0.0};
    }
    if (seam == "+y") {
        return {0.0, 1.0};
    }
    if (seam == "-y") {
        return {0.0, -1.0};
    }
    return {1.0, 0.0};
}

double normalizeDegrees(double degrees)
{
    double d = std::fmod(degrees, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    if (d >= 360.0) {
        d -= 360.0;
    }
    return d;
}

double cross2(const cv::Vec2d& a, const cv::Vec2d& b)
{
    return a[0] * b[1] - a[1] * b[0];
}

double dot2(const cv::Vec2d& a, const cv::Vec2d& b)
{
    return a[0] * b[0] + a[1] * b[1];
}

}  // namespace

bool isValidSeam(std::string_view seam)
{
    return seam == "+x" || seam == "-x" || seam == "+y" || seam == "-y";
}

bool isValidScaleSource(std::string_view source)
{
    return source == "stamped_dimensions" || source == "stamped_voxel_size" || source == "inferred_from_grid";
}

cv::Vec2d CompiledArc::centerAt(double z) const
{
    if (centers.empty()) {
        return {0.0, 0.0};
    }
    const long long index = std::clamp<long long>(std::llround(z), 0, static_cast<long long>(centers.size()) - 1);
    return centers[static_cast<std::size_t>(index)];
}

cv::Vec2d CompiledArc::direction(double thetaDeg) const
{
    const double t = thetaDeg * kDegToRad;
    const double c = std::cos(t);
    const double s = std::sin(t);
    return {seam[0] * c - seam[1] * s, seam[0] * s + seam[1] * c};
}

std::optional<CompiledArc> compileArc(const RayArc& arc, const Frame& frame, std::string* error)
{
    const auto fail = [error](std::string message) -> std::optional<CompiledArc> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    if (arc.umbilicus.controlPoints.empty()) {
        return fail("the ray arc has no umbilicus points");
    }
    const cv::Vec3i extent = frameExtentXyz(frame);
    if (extent[0] <= 0 || extent[1] <= 0 || extent[2] <= 0) {
        return fail("the region has no grid");
    }
    std::vector<cv::Vec3f> points;
    points.reserve(arc.umbilicus.controlPoints.size());
    for (const auto& p : arc.umbilicus.controlPoints) {
        points.emplace_back(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    }
    try {
        const auto umbilicus = vc::core::util::Umbilicus::FromPoints(std::move(points), cv::Vec3i(extent[2], extent[1], extent[0]));
        CompiledArc compiled;
        compiled.seam = seamVector(arc.umbilicus.seam);
        compiled.centers.reserve(umbilicus.centers().size());
        for (const auto& c : umbilicus.centers()) {
            compiled.centers.emplace_back(c[0], c[1]);
        }
        return compiled;
    } catch (const std::exception& e) {
        return fail(std::string("the umbilicus could not be built: ") + e.what());
    }
}

double radiusXY(const CompiledArc& compiled, const cv::Vec3d& regionPoint)
{
    const cv::Vec2d c = compiled.centerAt(regionPoint[2]);
    return std::hypot(regionPoint[0] - c[0], regionPoint[1] - c[1]);
}

double thetaDeg(const CompiledArc& compiled, const cv::Vec3d& regionPoint)
{
    const cv::Vec2d c = compiled.centerAt(regionPoint[2]);
    const cv::Vec2d q(regionPoint[0] - c[0], regionPoint[1] - c[1]);
    if (q[0] == 0.0 && q[1] == 0.0) {
        return 0.0;
    }
    const double angle = std::atan2(cross2(compiled.seam, q), dot2(compiled.seam, q)) * kRadToDeg;
    return angle < 0.0 ? angle + 360.0 : (angle >= 360.0 ? angle - 360.0 : angle);
}

bool inSector(const RayArc& arc, double theta)
{
    if (arc.thetaSweepDeg >= 360.0) {
        return true;
    }
    return normalizeDegrees(theta - arc.thetaStartDeg) <= arc.thetaSweepDeg;
}

bool contains(const RayArc& arc, const CompiledArc& compiled, const cv::Vec3d& regionPoint)
{
    if (!(regionPoint[2] >= arc.zMin && regionPoint[2] <= arc.zMax)) {
        return false;
    }
    const double r = radiusXY(compiled, regionPoint);
    if (!(r >= arc.radiusMin && r <= arc.radiusMax)) {
        return false;
    }
    return inSector(arc, thetaDeg(compiled, regionPoint));
}

namespace
{

// Angular span of an XY rectangle seen from a point outside it: the minimal
// arc covering the four corner angles, as (start, sweep) in degrees from the
// seam. Under 180 degrees because the point is outside.
std::pair<double, double> rectangleSpan(const CompiledArc& compiled, const cv::Vec2d& c, double x0, double x1, double y0, double y1)
{
    std::array<double, 4> angles{};
    const std::array<cv::Vec2d, 4> corners = {cv::Vec2d(x0, y0), cv::Vec2d(x1, y0), cv::Vec2d(x0, y1), cv::Vec2d(x1, y1)};
    for (int i = 0; i < 4; ++i) {
        const cv::Vec2d q = corners[i] - c;
        double a = std::atan2(cross2(compiled.seam, q), dot2(compiled.seam, q)) * kRadToDeg;
        angles[i] = a < 0.0 ? a + 360.0 : a;
    }
    std::sort(angles.begin(), angles.end());
    // The minimal covering arc starts after the largest gap.
    double bestGap = -1.0;
    int bestIndex = 0;
    for (int i = 0; i < 4; ++i) {
        const double next = i == 3 ? angles[0] + 360.0 : angles[i + 1];
        const double gap = next - angles[i];
        if (gap > bestGap) {
            bestGap = gap;
            bestIndex = i;
        }
    }
    const double start = angles[(bestIndex + 1) % 4];
    return {start, 360.0 - bestGap};
}

// Whether the arc (spanStart, spanSweep) and the sector intersect, and whether
// it lies inside the sector, both conservatively padded by `pad` degrees.
bool spansIntersect(double sectorStart, double sectorSweep, double spanStart, double spanSweep, double pad)
{
    if (sectorSweep >= 360.0) {
        return true;
    }
    // Two arcs intersect when either start lies inside the other arc.
    const double a = normalizeDegrees(spanStart - sectorStart);
    const double b = normalizeDegrees(sectorStart - spanStart);
    return a <= sectorSweep + pad || b <= spanSweep + pad || a >= 360.0 - pad || b >= 360.0 - pad;
}

bool spanInside(double sectorStart, double sectorSweep, double spanStart, double spanSweep, double pad)
{
    if (sectorSweep >= 360.0) {
        return true;
    }
    const double a = normalizeDegrees(spanStart - sectorStart);
    return a >= pad && a + spanSweep <= sectorSweep - pad;
}

}  // namespace

Coverage classify(const RayArc& arc, const CompiledArc& compiled, const cv::Vec3d& lo, const cv::Vec3d& hi)
{
    if (hi[2] < arc.zMin || lo[2] > arc.zMax) {
        return Coverage::Outside;
    }
    const long long n = static_cast<long long>(compiled.centers.size());
    if (n == 0) {
        return Coverage::Partial;
    }
    const long long k0 = std::clamp<long long>(std::llround(lo[2]), 0, n - 1);
    const long long k1 = std::clamp<long long>(std::llround(hi[2]), 0, n - 1);
    // Angular comparisons carry a small pad so rounding can only make a block
    // Partial, never wrongly Inside or Outside.
    constexpr double kPadDeg = 1e-7;
    constexpr double kPadR = 1e-9;
    bool allTooFar = true;
    bool allTooNear = true;
    bool allMissSector = true;
    bool allInside = lo[2] >= arc.zMin && hi[2] <= arc.zMax;
    for (long long k = k0; k <= k1; ++k) {
        const cv::Vec2d c = compiled.centers[static_cast<std::size_t>(k)];
        const double nx = std::clamp(c[0], lo[0], hi[0]);
        const double ny = std::clamp(c[1], lo[1], hi[1]);
        const double nearest = std::hypot(nx - c[0], ny - c[1]);
        const double farthest =
            std::hypot(std::max(std::abs(lo[0] - c[0]), std::abs(hi[0] - c[0])), std::max(std::abs(lo[1] - c[1]), std::abs(hi[1] - c[1])));
        const bool centerInside = c[0] >= lo[0] && c[0] <= hi[0] && c[1] >= lo[1] && c[1] <= hi[1];
        if (nearest <= arc.radiusMax + kPadR) {
            allTooFar = false;
        }
        if (farthest >= arc.radiusMin - kPadR) {
            allTooNear = false;
        }
        bool missesSector = false;
        bool insideSector = false;
        if (!centerInside) {
            const auto [spanStart, spanSweep] = rectangleSpan(compiled, c, lo[0], hi[0], lo[1], hi[1]);
            missesSector = !spansIntersect(arc.thetaStartDeg, arc.thetaSweepDeg, spanStart, spanSweep, kPadDeg);
            insideSector = spanInside(arc.thetaStartDeg, arc.thetaSweepDeg, spanStart, spanSweep, kPadDeg);
        } else if (arc.thetaSweepDeg >= 360.0) {
            insideSector = true;
        }
        if (!missesSector) {
            allMissSector = false;
        }
        if (!(nearest >= arc.radiusMin + kPadR && farthest <= arc.radiusMax - kPadR && !centerInside && insideSector)) {
            allInside = false;
        }
    }
    if (allTooFar || allTooNear || allMissSector) {
        return Coverage::Outside;
    }
    return allInside ? Coverage::Inside : Coverage::Partial;
}

std::vector<std::vector<cv::Vec3d>> arcOutlineHorizontal(const RayArc& arc, const CompiledArc& compiled, double zRegion, double stepDeg)
{
    std::vector<std::vector<cv::Vec3d>> paths;
    if (!(zRegion >= arc.zMin && zRegion <= arc.zMax) || compiled.centers.empty()) {
        return paths;
    }
    const double step = std::clamp(stepDeg, 0.05, 5.0);
    const cv::Vec2d c = compiled.centerAt(zRegion);
    const auto at = [&](double r, double t) {
        const cv::Vec2d d = compiled.direction(t);
        return cv::Vec3d(c[0] + r * d[0], c[1] + r * d[1], zRegion);
    };
    const auto arcPoints = [&](double r, double from, double sweep, std::vector<cv::Vec3d>& out) {
        const int n = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / step)));
        for (int i = 0; i <= n; ++i) {
            out.push_back(at(r, from + sweep * static_cast<double>(i) / n));
        }
    };
    if (arc.thetaSweepDeg >= 360.0) {
        for (const double r : {arc.radiusMax, arc.radiusMin}) {
            if (r <= 0.0) {
                continue;
            }
            std::vector<cv::Vec3d> circle;
            arcPoints(r, arc.thetaStartDeg, 360.0, circle);
            circle.pop_back();  // closed by the caller
            paths.push_back(std::move(circle));
        }
        return paths;
    }
    std::vector<cv::Vec3d> path;
    arcPoints(arc.radiusMax, arc.thetaStartDeg, arc.thetaSweepDeg, path);
    if (arc.radiusMin > 0.0) {
        arcPoints(arc.radiusMin, arc.thetaStartDeg + arc.thetaSweepDeg, -arc.thetaSweepDeg, path);
    } else {
        path.push_back(cv::Vec3d(c[0], c[1], zRegion));
    }
    paths.push_back(std::move(path));
    return paths;
}

std::vector<std::pair<double, double>> arcRowIntervals(const RayArc& arc, const CompiledArc& compiled, const cv::Vec2d& originXY, const cv::Vec2d& h, int slice)
{
    std::vector<std::pair<double, double>> out;
    if (slice < arc.zMin || slice > arc.zMax || compiled.centers.empty()) {
        return out;
    }
    const cv::Vec2d c = compiled.centerAt(static_cast<double>(slice));
    const cv::Vec2d q = originXY - c;
    const double qh = dot2(q, h);
    const double qq = dot2(q, q);
    const auto chord = [&](double r) -> std::optional<std::pair<double, double>> {
        const double disc = qh * qh - qq + r * r;
        if (r <= 0.0 || disc < 0.0) {
            return std::nullopt;
        }
        const double root = std::sqrt(disc);
        return std::pair<double, double>{-qh - root, -qh + root};
    };
    const auto outer = chord(arc.radiusMax);
    if (!outer) {
        return out;
    }
    std::vector<std::pair<double, double>> annulus;
    if (const auto inner = chord(arc.radiusMin); inner && inner->first < inner->second) {
        if (inner->first > outer->first) {
            annulus.emplace_back(outer->first, inner->first);
        }
        if (inner->second < outer->second) {
            annulus.emplace_back(inner->second, outer->second);
        }
    } else {
        annulus.push_back(*outer);
    }
    if (arc.thetaSweepDeg >= 360.0) {
        return annulus;
    }
    // Split at the line's crossings with the two boundary rays and keep the
    // pieces whose midpoint is inside the sector.
    std::vector<double> cuts;
    for (const double t : {arc.thetaStartDeg, arc.thetaStartDeg + arc.thetaSweepDeg}) {
        const cv::Vec2d d = compiled.direction(t);
        const double denom = cross2(d, h);
        if (std::abs(denom) < 1e-15) {
            continue;
        }
        const double u = -cross2(d, q) / denom;
        if (dot2(d, q + u * h) >= 0.0) {
            cuts.push_back(u);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    for (const auto& [a, b] : annulus) {
        std::vector<double> edges = {a};
        for (const double u : cuts) {
            if (u > a && u < b) {
                edges.push_back(u);
            }
        }
        edges.push_back(b);
        for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
            const double mid = 0.5 * (edges[i] + edges[i + 1]);
            const cv::Vec2d p = originXY + mid * h;
            const cv::Vec2d m = p - c;
            double theta = std::atan2(cross2(compiled.seam, m), dot2(compiled.seam, m)) * kRadToDeg;
            if (theta < 0.0) {
                theta += 360.0;
            }
            if (inSector(arc, theta)) {
                if (!out.empty() && std::abs(out.back().second - edges[i]) < 1e-12) {
                    out.back().second = edges[i + 1];
                } else {
                    out.emplace_back(edges[i], edges[i + 1]);
                }
            }
        }
    }
    return out;
}

namespace
{

// Lengths of [a, b] covered by exactly one of two interval sets.
std::vector<std::pair<double, double>> symmetricDifference(const std::vector<std::pair<double, double>>& a, const std::vector<std::pair<double, double>>& b)
{
    std::vector<std::pair<double, int>> events;
    for (const auto& [lo, hi] : a) {
        events.emplace_back(lo, 1);
        events.emplace_back(hi, -1);
    }
    for (const auto& [lo, hi] : b) {
        events.emplace_back(lo, 2);
        events.emplace_back(hi, -2);
    }
    std::sort(events.begin(), events.end());
    std::vector<std::pair<double, double>> out;
    int inA = 0;
    int inB = 0;
    double last = 0.0;
    bool have = false;
    for (const auto& [x, kind] : events) {
        const bool before = (inA > 0) != (inB > 0);
        if (have && before && x > last) {
            if (!out.empty() && std::abs(out.back().second - last) < 1e-12) {
                out.back().second = x;
            } else {
                out.emplace_back(last, x);
            }
        }
        if (kind == 1) {
            ++inA;
        } else if (kind == -1) {
            --inA;
        } else if (kind == 2) {
            ++inB;
        } else {
            --inB;
        }
        last = x;
        have = true;
    }
    return out;
}

}  // namespace

std::vector<std::array<cv::Vec3d, 2>> arcOutlineVertical(
    const RayArc& arc, const CompiledArc& compiled, const cv::Vec2d& originXY, const cv::Vec2d& h, int sliceFrom, int sliceTo, int rowStride)
{
    std::vector<std::array<cv::Vec3d, 2>> segments;
    const int stride = std::max(1, rowStride);
    const int from = std::max(sliceFrom, arc.zMin);
    const int to = std::min(sliceTo, arc.zMax);
    if (from > to) {
        return segments;
    }
    const auto point = [&](double u, double z) {
        const cv::Vec2d p = originXY + u * h;
        return cv::Vec3d(p[0], p[1], z);
    };
    // Rows of `stride` slices; row m covers [m - 0.5, m - 0.5 + stride).
    std::vector<std::pair<double, double>> previous;
    double previousTop = static_cast<double>(from) - 0.5;
    for (int m = from; m <= to; m += stride) {
        const double bottom = static_cast<double>(m) - 0.5;
        const double top = std::min(static_cast<double>(to) + 0.5, bottom + stride);
        const auto row = arcRowIntervals(arc, compiled, originXY, h, m);
        for (const auto& [a, b] : symmetricDifference(previous, row)) {
            segments.push_back({point(a, bottom), point(b, bottom)});
        }
        for (const auto& [a, b] : row) {
            segments.push_back({point(a, bottom), point(a, top)});
            segments.push_back({point(b, bottom), point(b, top)});
        }
        previous = row;
        previousTop = top;
    }
    for (const auto& [a, b] : previous) {
        segments.push_back({point(a, previousTop), point(b, previousTop)});
    }
    return segments;
}

std::vector<ArcHandle> arcHandles(const RayArc& arc, const CompiledArc& compiled, double zRegion)
{
    std::vector<ArcHandle> handles;
    if (!(zRegion >= arc.zMin && zRegion <= arc.zMax) || compiled.centers.empty()) {
        return handles;
    }
    const cv::Vec2d c = compiled.centerAt(zRegion);
    const auto at = [&](double r, double t) {
        const cv::Vec2d d = compiled.direction(t);
        return cv::Vec3d(c[0] + r * d[0], c[1] + r * d[1], zRegion);
    };
    const double midAngle = arc.thetaStartDeg + 0.5 * std::min(arc.thetaSweepDeg, 360.0);
    const double midRadius = 0.5 * (arc.radiusMin + arc.radiusMax);
    handles.push_back({ArcHandleKind::RadiusMin, at(arc.radiusMin, midAngle)});
    handles.push_back({ArcHandleKind::RadiusMax, at(arc.radiusMax, midAngle)});
    if (arc.thetaSweepDeg < 360.0) {
        handles.push_back({ArcHandleKind::ThetaStart, at(midRadius, arc.thetaStartDeg)});
        handles.push_back({ArcHandleKind::ThetaEnd, at(midRadius, arc.thetaStartDeg + arc.thetaSweepDeg)});
    }
    return handles;
}

namespace
{

double roundTo(double value, double quantum)
{
    return std::round(value / quantum) * quantum;
}

}  // namespace

RayArc dragArcHandle(const RayArc& start, const CompiledArc& compiled, ArcHandleKind kind, const cv::Vec3d& cursorRegion)
{
    RayArc arc = start;
    const double r = roundTo(radiusXY(compiled, cursorRegion), 0.01);
    const double t = roundTo(thetaDeg(compiled, cursorRegion), 0.01);
    switch (kind) {
        case ArcHandleKind::RadiusMin:
        case ArcHandleKind::RadiusMax: {
            const double other = kind == ArcHandleKind::RadiusMin ? start.radiusMax : start.radiusMin;
            double lo = std::min(r, other);
            double hi = std::max(r, other);
            if (hi - lo < kMinRadiusSpan) {
                if (r <= other) {
                    lo = other - kMinRadiusSpan;
                } else {
                    hi = other + kMinRadiusSpan;
                }
            }
            arc.radiusMin = std::max(0.0, lo);
            arc.radiusMax = std::max(arc.radiusMin + kMinRadiusSpan, hi);
            break;
        }
        case ArcHandleKind::ThetaStart: {
            const double end = start.thetaStartDeg + start.thetaSweepDeg;
            double sweep = normalizeDegrees(end - t);
            if (sweep < kMinSweepDeg) {
                sweep = kMinSweepDeg;
            }
            arc.thetaStartDeg = normalizeDegrees(end - sweep);
            arc.thetaSweepDeg = roundTo(sweep, 0.01);
            break;
        }
        case ArcHandleKind::ThetaEnd: {
            double sweep = normalizeDegrees(t - start.thetaStartDeg);
            if (sweep < kMinSweepDeg) {
                sweep = kMinSweepDeg;
            }
            arc.thetaSweepDeg = roundTo(sweep, 0.01);
            break;
        }
    }
    arc.thetaStartDeg = normalizeDegrees(roundTo(arc.thetaStartDeg, 0.01));
    return arc;
}

std::optional<RayArc> arcFromClicks(
    const CompiledArc& compiled, const cv::Vec3d& firstRegion, const cv::Vec3d& secondRegion, bool increasing, double sliceRegion, int depthVoxels, int zMaxIndex)
{
    const double rA = radiusXY(compiled, firstRegion);
    const double rB = radiusXY(compiled, secondRegion);
    if (std::abs(rA - rB) < kMinRadiusSpan) {
        return std::nullopt;
    }
    const double tA = thetaDeg(compiled, firstRegion);
    const double tB = thetaDeg(compiled, secondRegion);
    RayArc arc;
    const double start = increasing ? tA : tB;
    const double end = increasing ? tB : tA;
    double sweep = normalizeDegrees(end - start);
    if (sweep < kMinSweepDeg) {
        sweep = 360.0;
    }
    arc.thetaStartDeg = normalizeDegrees(roundTo(start, 0.01));
    arc.thetaSweepDeg = std::min(360.0, roundTo(sweep, 0.01));
    arc.radiusMin = roundTo(std::min(rA, rB), 0.01);
    arc.radiusMax = roundTo(std::max(rA, rB), 0.01);
    const long long depth = std::max(1, depthVoxels);
    const long long lo = std::llround(sliceRegion) - depth / 2;
    const long long hi = lo + depth - 1;
    arc.zMin = static_cast<int>(std::clamp<long long>(lo, 0, zMaxIndex));
    arc.zMax = static_cast<int>(std::clamp<long long>(hi, 0, zMaxIndex));
    if (arc.zMin > arc.zMax) {
        return std::nullopt;
    }
    return arc;
}

std::array<cv::Vec3d, 2> arcBounds(const RayArc& arc, const CompiledArc& compiled)
{
    const long long n = static_cast<long long>(compiled.centers.size());
    if (n == 0) {
        return {cv::Vec3d(0, 0, arc.zMin), cv::Vec3d(-1, -1, arc.zMax)};
    }
    // The sector's extent around its center: its four corners, plus the
    // outer radius in each axis direction the sector contains.
    double rx0 = std::numeric_limits<double>::infinity();
    double ry0 = rx0;
    double rx1 = -rx0;
    double ry1 = -rx0;
    const auto include = [&](const cv::Vec2d& p) {
        rx0 = std::min(rx0, p[0]);
        rx1 = std::max(rx1, p[0]);
        ry0 = std::min(ry0, p[1]);
        ry1 = std::max(ry1, p[1]);
    };
    if (arc.thetaSweepDeg >= 360.0) {
        include({-arc.radiusMax, -arc.radiusMax});
        include({arc.radiusMax, arc.radiusMax});
    } else {
        for (const double t : {arc.thetaStartDeg, arc.thetaStartDeg + arc.thetaSweepDeg}) {
            const cv::Vec2d d = compiled.direction(t);
            include(arc.radiusMin * d);
            include(arc.radiusMax * d);
        }
        for (const cv::Vec2d axis : {cv::Vec2d(1, 0), cv::Vec2d(0, 1), cv::Vec2d(-1, 0), cv::Vec2d(0, -1)}) {
            double t = std::atan2(cross2(compiled.seam, axis), dot2(compiled.seam, axis)) * kRadToDeg;
            if (inSector(arc, t < 0.0 ? t + 360.0 : t)) {
                include(arc.radiusMax * axis);
            }
        }
    }
    double cx0 = std::numeric_limits<double>::infinity();
    double cy0 = cx0;
    double cx1 = -cx0;
    double cy1 = -cx0;
    const long long k0 = std::clamp<long long>(arc.zMin, 0, n - 1);
    const long long k1 = std::clamp<long long>(arc.zMax, 0, n - 1);
    for (long long k = k0; k <= k1; ++k) {
        const cv::Vec2d c = compiled.centers[static_cast<std::size_t>(k)];
        cx0 = std::min(cx0, c[0]);
        cx1 = std::max(cx1, c[0]);
        cy0 = std::min(cy0, c[1]);
        cy1 = std::max(cy1, c[1]);
    }
    return {cv::Vec3d(cx0 + rx0, cy0 + ry0, arc.zMin), cv::Vec3d(cx1 + rx1, cy1 + ry1, arc.zMax)};
}

}  // namespace vc3d::regions
