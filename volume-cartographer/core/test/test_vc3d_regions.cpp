#include <doctest/doctest.h>

#include "RegionModel.hpp"
#include "RegionStore.hpp"
#include "volume_viewers/CVolumeViewerView.hpp"
#include "volume_viewers/PointerGate.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLockFile>
#include <QTest>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace regions = vc3d::regions;

namespace
{

fs::path tempRoot(const std::string& tag)
{
    std::mt19937_64 rng(std::random_device{}());
    auto path = fs::temp_directory_path() / ("vc_regions_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(path);
    return path;
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

regions::Frame testFrame()
{
    regions::Frame frame;
    frame.baseShapeZyx = {200, 120, 100};  // z y x
    frame.voxelSizeUm = 7.91;
    frame.volumeId = "test-volume";
    frame.coordinateSpace = "PHerc0172/20241024131838@L0";
    frame.coordinateIdentity = regions::Json::object();
    frame.coordinateIdentity["vc_open_data_coordinate_space"] = frame.coordinateSpace;
    frame.coordinateIdentity["vc_open_data_source_path"] = "s3://bucket/volume.zarr";
    frame.coordinateIdentity["vc_open_data_source_coordinate_level"] = 0;
    frame.coordinateIdentity["vc_open_data_source_coordinate_scale_factor"] = 1;
    frame.coordinateIdentity["vc_open_data_source_original_resolution"] = 7.91;
    return frame;
}

regions::Region testRegion(const std::string& id = "region-1")
{
    regions::Region region;
    region.id = id;
    region.name = "sheet jump near x 40";
    region.color = "#ffb000";
    region.tags = {"sheet_jump"};
    region.notes = "prediction jumps one sheet outward";
    region.author = "tester";
    region.created = "2026-10-18T14:03:11Z";
    region.modified = "2026-10-18T14:03:11Z";
    region.revision = 1;
    region.frame = testFrame();
    regions::Box box;
    box.min = {10, 20, 30};
    box.max = {40, 50, 60};
    region.shape = box;
    return region;
}

vc3d::annotation::AnnotationFrame annotationFrame(double x, double y, double z, double factor, std::optional<double> voxel)
{
    vc3d::annotation::AnnotationFrame frame;
    frame.extentXyz = {x, y, z};
    frame.factor = factor;
    frame.voxelSizeUm = voxel;
    return frame;
}

regions::Plane axisPlane(int axis, double value)
{
    regions::Plane plane;
    plane.origin = {0, 0, 0};
    plane.origin[axis] = value;
    plane.normal = {0, 0, 0};
    plane.normal[axis] = 1;
    if (axis == 2) {
        plane.basisX = {1, 0, 0};
        plane.basisY = {0, 1, 0};
    } else if (axis == 1) {
        plane.basisX = {1, 0, 0};
        plane.basisY = {0, 0, 1};
    } else {
        plane.basisX = {0, 1, 0};
        plane.basisY = {0, 0, 1};
    }
    return plane;
}

regions::Plane planeFromNormal(const cv::Vec3d& origin, cv::Vec3d normal)
{
    normal /= cv::norm(normal);
    cv::Vec3d helper = std::abs(normal[2]) < 0.9 ? cv::Vec3d(0, 0, 1) : cv::Vec3d(1, 0, 0);
    cv::Vec3d bx = helper.cross(normal);
    bx /= cv::norm(bx);
    const cv::Vec3d by = normal.cross(bx);
    return {origin, normal, bx, by};
}

std::string errorPathOf(const regions::Json& document)
{
    const auto parsed = regions::parseRegion(document);
    return parsed.loaded ? std::string("<loaded>") : parsed.errorPath;
}

}  // namespace

TEST_CASE("Region JSON round trips byte for byte and names the first bad field")
{
    const regions::Region region = testRegion();
    const std::string text = regions::toJson(region).dump(2);
    const auto parsed = regions::parseRegion(regions::Json::parse(text));
    REQUIRE(parsed.loaded.has_value());
    CHECK_FALSE(parsed.loaded->readOnly);
    CHECK(regions::toJson(parsed.loaded->region).dump(2) == text);
    REQUIRE(regions::boxOf(parsed.loaded->region));
    CHECK(*regions::boxOf(parsed.loaded->region) == *regions::boxOf(region));
    CHECK(parsed.loaded->region.frame.coordinateSpace == region.frame.coordinateSpace);
    CHECK(parsed.loaded->region.frame.baseShapeZyx == region.frame.baseShapeZyx);

    // The document starts with the schema, so files read naturally.
    CHECK(text.find("\"schema\"") < text.find("\"version\""));

    const regions::Json good = regions::toJson(region);

    regions::Json doc = good;
    doc.erase("schema");
    CHECK(errorPathOf(doc) == "schema");

    doc = good;
    doc["version"] = 0;
    CHECK(errorPathOf(doc) == "version");

    doc = good;
    doc["shape"]["max"]["z"] = 60.5;
    CHECK(errorPathOf(doc) == "shape.max.z");

    doc = good;
    doc["shape"]["min"]["y"] = 51;
    CHECK(errorPathOf(doc) == "shape.max.y");

    doc = good;
    doc["shape"]["max"]["x"] = 100;  // extent x is 100: largest index 99
    CHECK(errorPathOf(doc) == "shape.max.x");

    doc = good;
    doc["color"] = "orange";
    CHECK(errorPathOf(doc) == "color");

    doc = good;
    doc["status"] = "pending";
    CHECK(errorPathOf(doc) == "status");

    doc = good;
    doc["id"] = "../escape";
    CHECK(errorPathOf(doc) == "id");

    doc = good;
    doc["frame"]["coordinate_base_shape_zyx"] = regions::Json::array({200, 120});
    CHECK(errorPathOf(doc) == "frame.coordinate_base_shape_zyx");

    doc = good;
    doc["shape"]["type"] = "sphere";
    CHECK(errorPathOf(doc) == "shape.type");
    CHECK(regions::parseRegion(doc).error.find("unknown shape type") != std::string::npos);
    // The same shape type in a newer file says where it came from.
    doc["version"] = 2;
    CHECK(errorPathOf(doc) == "shape.type");
    CHECK(regions::parseRegion(doc).error.find("newer VC3D (region version 2)") != std::string::npos);

    doc = good;
    doc["version"] = 2;
    auto newer = regions::parseRegion(doc);
    REQUIRE(newer.loaded.has_value());
    CHECK(newer.loaded->readOnly);
    CHECK(newer.loaded->readOnlyReason.find("newer VC3D") != std::string::npos);

    doc = good;
    doc["foo"] = 1;
    auto unknown = regions::parseRegion(doc);
    REQUIRE(unknown.loaded.has_value());
    CHECK(unknown.loaded->readOnly);
    CHECK(unknown.loaded->readOnlyReason == "unknown field `foo`");

    doc = good;
    doc["shape"]["min"]["w"] = 1;
    unknown = regions::parseRegion(doc);
    REQUIRE(unknown.loaded.has_value());
    CHECK(unknown.loaded->readOnlyReason == "unknown field `shape.min.w`");
}

TEST_CASE("Region frames map onto volumes of the same scan and refuse others")
{
    const regions::Frame frame = testFrame();
    const std::string space = frame.coordinateSpace;

    SUBCASE("same frame")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(100, 120, 200, 1.0, 7.91), space);
        REQUIRE(check.map.has_value());
        CHECK(check.map->log2RegionToViewer == 0);
        CHECK(check.map->regionToViewer() == 1.0);
    }
    SUBCASE("a rebased store sees the same annotation voxels at a quarter of the viewer coordinates")
    {
        // baseScaleLevel 2: the store is a quarter of the grid, factor 4,
        // annotation extent the full grid.
        const auto check = regions::mapRegionFrame(frame, annotationFrame(100, 120, 200, 4.0, 7.91), space);
        REQUIRE(check.map.has_value());
        CHECK(check.map->regionToViewer() == 0.25);
        const regions::Box box = *regions::boxOf(testRegion());
        const Rect3D bounds = regions::focusBounds(box, *check.map);
        CHECK(bounds.low[0] == doctest::Approx(10 * 0.25));
        CHECK(bounds.high[2] == doctest::Approx(60 * 0.25));
    }
    SUBCASE("a rebased store whose level rounds the grid up still maps")
    {
        // A 101 x 121 x 201 source stored at level 2 has 26 x 31 x 51 voxels,
        // so its annotation extent is 104 x 124 x 204, not the source's.
        regions::Frame odd = frame;
        odd.baseShapeZyx = {201, 121, 101};
        const auto check = regions::mapRegionFrame(odd, annotationFrame(104, 124, 204, 4.0, 7.91), space);
        REQUIRE_MESSAGE(check.map.has_value(), check.reason);
        CHECK(check.map->regionToViewer() == 0.25);
        // Control: one level-2 voxel more on an axis is another grid.
        CHECK_FALSE(regions::mapRegionFrame(odd, annotationFrame(108, 124, 204, 4.0, 7.91), space).map.has_value());
    }
    SUBCASE("a region drawn on a rebased store maps back to full resolution")
    {
        regions::Frame quarter = frame;
        quarter.baseShapeZyx = {50, 30, 25};
        quarter.voxelSizeUm = 7.91 * 4;
        const auto check = regions::mapRegionFrame(quarter, annotationFrame(100, 120, 200, 1.0, 7.91), space);
        REQUIRE(check.map.has_value());
        CHECK(check.map->regionToViewer() == 4.0);
    }
    SUBCASE("same-level extents one voxel apart are accepted")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(101, 120, 199, 1.0, 7.91), space);
        REQUIRE(check.map.has_value());
        CHECK(check.map->log2RegionToViewer == 0);
    }
    SUBCASE("conflicting coordinate space")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(100, 120, 200, 1.0, 7.91), "PHerc0139/other@L0");
        CHECK_FALSE(check.map.has_value());
        CHECK(check.reason.find("another scan") != std::string::npos);
    }
    SUBCASE("an untagged volume is not a conflict")
    {
        CHECK(regions::mapRegionFrame(frame, annotationFrame(100, 120, 200, 1.0, 7.91), "").map.has_value());
    }
    SUBCASE("voxel size mismatch")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(100, 120, 200, 1.0, 3.95), space);
        CHECK_FALSE(check.map.has_value());
        CHECK(check.reason.find("Voxel size differs") != std::string::npos);
    }
    SUBCASE("non-dyadic extent")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(70, 120, 200, 1.0, std::nullopt), space);
        CHECK_FALSE(check.map.has_value());
        CHECK(check.reason.find("Grid does not match") != std::string::npos);
    }
    SUBCASE("unknown volume frame")
    {
        const auto check = regions::mapRegionFrame(frame, annotationFrame(0, 0, 0, 1.0, std::nullopt), space);
        CHECK_FALSE(check.map.has_value());
    }
    SUBCASE("frameForVolume records the volume's extent and identity")
    {
        const auto made = regions::frameForVolume(annotationFrame(100, 120, 200, 2.0, 3.0), "vol", frame.coordinateIdentity);
        CHECK(made.baseShapeZyx == std::array<long long, 3>{200, 120, 100});
        CHECK(made.coordinateSpace == space);
        CHECK(made.volumeId == "vol");
        REQUIRE(made.voxelSizeUm.has_value());
        CHECK(*made.voxelSizeUm == 3.0);
        CHECK(regions::mapRegionFrame(made, annotationFrame(100, 120, 200, 2.0, 3.0), space).map->regionToViewer() == 0.5);
    }
}

TEST_CASE("Box cross-sections are exact on axis planes and stay on the face box")
{
    const regions::Box box{{10, 20, 30}, {40, 50, 60}};
    const regions::FrameMap identity{0};

    const auto xy = regions::boxCrossSection(box, identity, axisPlane(2, 45.0));
    REQUIRE(xy.size() == 4);
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& p : xy) {
        minX = std::min(minX, p[0]);
        maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]);
        maxY = std::max(maxY, p[1]);
        CHECK(p[2] == doctest::Approx(45.0));
    }
    CHECK(minX == doctest::Approx(9.5));
    CHECK(maxX == doctest::Approx(40.5));
    CHECK(minY == doctest::Approx(19.5));
    CHECK(maxY == doctest::Approx(50.5));

    // Voxel faces: the plane at the last voxel's face still cuts, beyond it not.
    CHECK(regions::boxCrossSection(box, identity, axisPlane(2, 60.4)).size() == 4);
    CHECK(regions::boxCrossSection(box, identity, axisPlane(2, 61.0)).empty());
    CHECK(regions::boxCrossSection(box, identity, axisPlane(0, 5.0)).empty());

    // A quarter-scale map shrinks the outline exactly.
    const auto quarter = regions::boxCrossSection(box, regions::FrameMap{-2}, axisPlane(2, 45.0 / 4.0));
    REQUIRE(quarter.size() == 4);
    for (const auto& p : quarter) {
        CHECK((p[0] == doctest::Approx(9.5 / 4.0) || p[0] == doctest::Approx(40.5 / 4.0)));
    }

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::uniform_real_distribution<double> inside(0.0, 70.0);
    int cut = 0;
    for (int i = 0; i < 1000; ++i) {
        cv::Vec3d normal(unit(rng), unit(rng), unit(rng));
        if (cv::norm(normal) < 1e-3) {
            continue;
        }
        const regions::Plane plane = planeFromNormal({inside(rng), inside(rng), inside(rng)}, normal);
        const auto section = regions::boxCrossSection(box, identity, plane);
        CHECK((section.empty() || (section.size() >= 3 && section.size() <= 6)));
        if (!section.empty()) {
            ++cut;
        }
        const cv::Vec3d n = plane.normal;
        for (const auto& p : section) {
            CHECK(std::abs((p - plane.origin).dot(n)) < 1e-6);
            bool onFace = false;
            for (int axis = 0; axis < 3; ++axis) {
                const double lo = box.min[axis] - 0.5;
                const double hi = box.max[axis] + 0.5;
                CHECK(p[axis] >= lo - 1e-6);
                CHECK(p[axis] <= hi + 1e-6);
                if (std::abs(p[axis] - lo) < 1e-6 || std::abs(p[axis] - hi) < 1e-6) {
                    onFace = true;
                }
            }
            CHECK(onFace);
        }
    }
    CHECK(cut > 100);

    CHECK(regions::classifyPlane(axisPlane(2, 0)) == regions::PlaneKind::AxisZ);
    CHECK(regions::classifyPlane(axisPlane(1, 0)) == regions::PlaneKind::AxisY);
    CHECK(regions::classifyPlane(axisPlane(0, 0)) == regions::PlaneKind::AxisX);
    CHECK(regions::classifyPlane(planeFromNormal({0, 0, 0}, {std::cos(0.5), std::sin(0.5), 0})) == regions::PlaneKind::Vertical);
    CHECK(regions::classifyPlane(planeFromNormal({0, 0, 0}, {0.3, 0.2, 0.9})) == regions::PlaneKind::Tilted);
}

TEST_CASE("Box handles move only the axes their plane can edit")
{
    const regions::Box box{{10, 20, 30}, {40, 50, 60}};
    const regions::FrameMap identity{0};
    const cv::Vec3i extentMax(99, 119, 199);

    SUBCASE("XY corner moves x and y")
    {
        const regions::Plane plane = axisPlane(2, 45.0);
        const auto handles = regions::boxHandles(box, identity, plane, regions::PlaneKind::AxisZ);
        CHECK(handles.size() == 9);
        const regions::Handle* corner = nullptr;
        for (const auto& h : handles) {
            if (h.kind == regions::HandleKind::Corner && h.sides[0] > 0 && h.sides[1] > 0) {
                corner = &h;
            }
        }
        REQUIRE(corner != nullptr);
        CHECK(corner->viewerPos[0] == doctest::Approx(40.5));
        CHECK(corner->viewerPos[1] == doctest::Approx(50.5));
        CHECK(corner->viewerPos[2] == doctest::Approx(45.0));
        const regions::Box moved =
            regions::dragHandle(box, *corner, corner->viewerPos, corner->viewerPos + cv::Vec3d(5.2, -3.4, 9.0), identity, extentMax);
        CHECK(moved.max[0] == 45);
        CHECK(moved.max[1] == 47);
        CHECK(moved.min == box.min);
        CHECK(moved.max[2] == box.max[2]);
    }
    SUBCASE("XZ top edge moves z only")
    {
        const regions::Plane plane = axisPlane(1, 35.0);
        const auto handles = regions::boxHandles(box, identity, plane, regions::PlaneKind::AxisY);
        const regions::Handle* top = nullptr;
        for (const auto& h : handles) {
            if (h.kind == regions::HandleKind::Edge && h.axes[0] == 2 && h.sides[0] > 0) {
                top = &h;
            }
        }
        REQUIRE(top != nullptr);
        const regions::Box moved = regions::dragHandle(box, *top, top->viewerPos, top->viewerPos + cv::Vec3d(7.0, 0.0, 12.0), identity, extentMax);
        CHECK(moved.max[2] == 72);
        CHECK(moved.min == box.min);
        CHECK(moved.max[0] == box.max[0]);
        CHECK(moved.max[1] == box.max[1]);
    }
    SUBCASE("a vertical plane rotated by 30 degrees has only z handles")
    {
        const double angle = 30.0 * M_PI / 180.0;
        const regions::Plane plane = planeFromNormal({25, 35, 45}, {std::cos(angle), std::sin(angle), 0});
        const auto handles = regions::boxHandles(box, identity, plane, regions::classifyPlane(plane));
        REQUIRE(handles.size() == 2);
        for (const auto& h : handles) {
            CHECK(h.axes[0] == 2);
            CHECK(h.axes[1] == -1);
        }
        CHECK(regions::boxHandles(box, identity, planeFromNormal({25, 35, 45}, {0.3, 0.2, 0.9}), regions::PlaneKind::Tilted).empty());
    }
    SUBCASE("dragging past the opposite face swaps, results are clamped and integer")
    {
        regions::Handle maxX{regions::HandleKind::Edge, {0, -1}, {1, 0}, {40.5, 35, 45}};
        const regions::Box swapped = regions::dragHandle(box, maxX, {40.5, 35, 45}, {0.2, 35, 45}, identity, extentMax);
        CHECK(swapped.min[0] == 0);
        CHECK(swapped.max[0] == 10);
        const regions::Box clamped = regions::dragHandle(box, maxX, {40.5, 35, 45}, {500.0, 35, 45}, identity, extentMax);
        CHECK(clamped.max[0] == 99);
        const regions::Handle* body = nullptr;
        const auto handles = regions::boxHandles(box, identity, axisPlane(2, 45.0), regions::PlaneKind::AxisZ);
        for (const auto& h : handles) {
            if (h.kind == regions::HandleKind::Body) {
                body = &h;
            }
        }
        // The center handle sits at the middle of the outline, on the slice.
        REQUIRE(body != nullptr);
        CHECK(body->viewerPos[0] == doctest::Approx(25.0));
        CHECK(body->viewerPos[1] == doctest::Approx(35.0));
        CHECK(body->viewerPos[2] == doctest::Approx(45.0));
        const regions::Box shifted = regions::dragHandle(box, *body, {20, 30, 45}, {1000, 31.6, 45}, identity, extentMax);
        CHECK(shifted.max[0] == 99);
        CHECK(shifted.max[0] - shifted.min[0] == box.max[0] - box.min[0]);
        CHECK(shifted.min[1] == 22);
        CHECK(shifted.min[2] == box.min[2]);
    }
}

TEST_CASE("New boxes take their depth centered on the slice")
{
    const regions::FrameMap identity{0};
    const cv::Vec3i extentMax(99, 119, 199);
    const auto box = regions::boxFromDrag({10.2, 20.6, 0}, {30.4, 5.0, 0}, regions::PlaneKind::AxisZ, {0, 0, 100}, 64, identity, extentMax);
    REQUIRE(box.has_value());
    CHECK(box->min == cv::Vec3i(10, 5, 68));
    CHECK(box->max == cv::Vec3i(30, 21, 131));

    const auto edge = regions::boxFromDrag({10, 20, 0}, {30, 40, 0}, regions::PlaneKind::AxisZ, {0, 0, 3}, 64, identity, extentMax);
    REQUIRE(edge.has_value());
    CHECK(edge->min[2] == 0);
    CHECK(edge->max[2] == 34);

    CHECK_FALSE(regions::boxFromDrag({10, 20, 0}, {10.6, 40, 0}, regions::PlaneKind::AxisZ, {0, 0, 50}, 8, identity, extentMax).has_value());
    CHECK_FALSE(regions::boxFromDrag({10, 20, 0}, {30, 40, 0}, regions::PlaneKind::Vertical, {0, 0, 50}, 8, identity, extentMax).has_value());

    // XZ plane: x and z from the drag, y from the slice.
    const auto xz = regions::boxFromDrag({10, 60, 20}, {20, 60, 40}, regions::PlaneKind::AxisY, {0, 60, 0}, 3, identity, extentMax);
    REQUIRE(xz.has_value());
    CHECK(xz->min == cv::Vec3i(10, 59, 20));
    CHECK(xz->max == cv::Vec3i(20, 61, 40));

    // A viewer at half the annotation resolution doubles the region coordinates.
    const auto half = regions::boxFromDrag({5, 10, 0}, {15, 20, 0}, regions::PlaneKind::AxisZ, {0, 0, 50}, 4, regions::FrameMap{-1}, extentMax);
    REQUIRE(half.has_value());
    CHECK(half->min == cv::Vec3i(10, 20, 98));
    CHECK(half->max == cv::Vec3i(30, 40, 101));

    CHECK(regions::voxelCount(*xz) == 11u * 3u * 21u);
    CHECK(regions::describeBox(*xz) == "x 10..20  y 59..61  z 20..40  (11 x 3 x 21 voxels)");
    CHECK(regions::contains(*xz, {10, 59, 40}));
    CHECK_FALSE(regions::contains(*xz, {9.9, 60, 30}));
}

TEST_CASE("Region store writes, refuses stale revisions and trashes")
{
    const fs::path root = tempRoot("store");
    const fs::path dir = root / "regions" / "project.volpkg.json";
    int tick = 0;
    regions::RegionStore store;
    store.setClock([&tick]() { return "2026-10-18T14:00:" + std::to_string(10 + tick++) + "Z"; });
    store.setLockTimeoutMs(200);

    SUBCASE("no project writes nothing")
    {
        regions::RegionStore empty;
        CHECK(empty.create(testRegion()).code == regions::Code::NoProject);
        CHECK(empty.regions().empty());
    }

    store.setDirectory(dir);
    const auto created = store.create(testRegion());
    REQUIRE(created.ok());
    CHECK(created.region->revision == 1);
    const fs::path file = store.fileFor("region-1");
    REQUIRE(fs::exists(file));
    const std::string firstBytes = readFile(file);
    CHECK(firstBytes == regions::toJson(*created.region).dump(2) + "\n");

    SUBCASE("byte-identical round trip after one write")
    {
        const auto parsed = regions::parseRegion(regions::Json::parse(firstBytes));
        REQUIRE(parsed.loaded.has_value());
        CHECK(regions::toJson(parsed.loaded->region).dump(2) + "\n" == firstBytes);
    }
    SUBCASE("stale expected revision is refused and the file is untouched")
    {
        regions::Region next = created.region.value();
        next.name = "renamed";
        const auto result = store.update(next, 7);
        CHECK(result.code == regions::Code::Conflict);
        CHECK(result.param == QStringLiteral("expectedRevision"));
        REQUIRE(result.currentRevision.has_value());
        CHECK(*result.currentRevision == 1);
        CHECK(readFile(file) == firstBytes);
    }
    SUBCASE("another writer's revision on disk is a conflict even when memory agreed")
    {
        regions::Region other = created.region.value();
        other.revision = 2;
        other.name = "changed elsewhere";
        writeFile(file, regions::toJson(other).dump(2) + "\n");
        regions::Region next = created.region.value();
        next.name = "mine";
        const auto result = store.update(next, 1);
        CHECK(result.code == regions::Code::Conflict);
        REQUIRE(store.find("region-1") != nullptr);
        CHECK(store.find("region-1")->region.name == "changed elsewhere");
        CHECK(store.find("region-1")->region.revision == 2);
    }
    SUBCASE("update bumps the revision and keeps server-owned fields")
    {
        regions::Region next = created.region.value();
        next.name = "renamed";
        next.author = "someone else";
        regions::boxOf(next)->max[2] = 70;
        const auto result = store.update(next, 1);
        REQUIRE(result.ok());
        CHECK(result.region->revision == 2);
        CHECK(result.region->author == "tester");
        CHECK(regions::boxOf(store.find("region-1")->region)->max[2] == 70);
        CHECK(store.find("region-1")->region.name == "renamed");
    }
    SUBCASE("a dry run checks like a save and writes nothing")
    {
        regions::Region next = created.region.value();
        regions::boxOf(next)->max[2] = 70;
        const auto preview = store.update(next, 1, /*dryRun=*/true);
        REQUIRE(preview.ok());
        CHECK(preview.region->revision == 2);
        CHECK(readFile(file) == firstBytes);
        CHECK(store.find("region-1")->region.revision == 1);
        CHECK(store.update(next, 7, true).code == regions::Code::Conflict);
        regions::boxOf(next)->max[0] = 1000;
        CHECK(store.update(next, 1, true).param == QStringLiteral("shape.max.x"));
        regions::Region fresh = testRegion("region-2");
        const auto created2 = store.create(fresh, /*dryRun=*/true);
        REQUIRE(created2.ok());
        CHECK(created2.region->revision == 1);
        CHECK_FALSE(fs::exists(store.fileFor("region-2")));
        CHECK(store.find("region-2") == nullptr);
        CHECK(store.create(testRegion(), true).code == regions::Code::Conflict);
    }
    SUBCASE("delete moves the file to the trash")
    {
        const auto removed = store.remove("region-1", 1);
        REQUIRE(removed.ok());
        CHECK_FALSE(fs::exists(file));
        CHECK(fs::exists(store.trashDirectory() / file.filename()));
        CHECK(readFile(store.trashDirectory() / file.filename()) == firstBytes);
        CHECK(store.find("region-1") == nullptr);
        store.reload();
        CHECK(store.regions().empty());
    }
    SUBCASE("reload gives identical regions and ignores temporaries")
    {
        writeFile(dir / "region-1.region.json.AbCdEf", "{ not json");
        writeFile(dir / "notes.txt", "hello");
        regions::RegionStore other;
        other.setDirectory(dir);
        REQUIRE(other.regions().size() == 1);
        CHECK(other.loadErrors().empty());
        CHECK(regions::toJson(other.regions().front().region).dump(2) == regions::toJson(*created.region).dump(2));
    }
    SUBCASE("a malformed file is reported with its path, not loaded")
    {
        regions::Region bad = testRegion("bad-1");
        regions::Json doc = regions::toJson(bad);
        doc["shape"]["max"]["z"] = "high";
        writeFile(dir / "bad-1.region.json", doc.dump(2));
        store.reload();
        CHECK(store.regions().size() == 1);
        REQUIRE(store.loadErrors().size() == 1);
        CHECK(store.loadErrors().front().file == "bad-1.region.json");
        CHECK(store.loadErrors().front().path == "shape.max.z");
    }
    SUBCASE("a write failure changes nothing in memory")
    {
        // A regions folder under a regular file cannot be created, even by
        // root, which ignores permission bits.
        writeFile(root / "not-a-folder", "x");
        regions::RegionStore blocked;
        blocked.setDirectory(root / "not-a-folder" / "regions");
        const auto result = blocked.create(testRegion("region-2"));
        CHECK(result.code == regions::Code::Io);
        CHECK(blocked.regions().empty());
        // A path that already holds something else is never overwritten.
        fs::create_directories(dir / "region-3.region.json");
        CHECK(store.create(testRegion("region-3")).code == regions::Code::Conflict);
        CHECK(store.find("region-3") == nullptr);
        CHECK(store.regions().size() == 1);
    }
    SUBCASE("a busy lock is refused")
    {
        QLockFile other(QString::fromStdString((dir / ".regions.lock").string()));
        REQUIRE(other.tryLock(0));
        regions::Region next = created.region.value();
        next.name = "while locked";
        CHECK(store.update(next, 1).code == regions::Code::Busy);
        CHECK(readFile(file) == firstBytes);
        other.unlock();
    }
    SUBCASE("read-only regions are never rewritten")
    {
        regions::Json doc = regions::toJson(testRegion("newer-1"));
        doc["version"] = 2;
        doc["future_field"] = true;
        const std::string newerBytes = doc.dump(2);
        writeFile(dir / "newer-1.region.json", newerBytes);
        store.reload();
        REQUIRE(store.find("newer-1") != nullptr);
        CHECK(store.find("newer-1")->readOnly);
        regions::Region next = store.find("newer-1")->region;
        next.name = "edit";
        CHECK(store.update(next, std::nullopt).code == regions::Code::ReadOnly);
        CHECK(readFile(dir / "newer-1.region.json") == newerBytes);
    }
    SUBCASE("invalid shapes are refused with the field")
    {
        regions::Region next = created.region.value();
        regions::boxOf(next)->max[0] = 1000;
        const auto result = store.update(next, 1);
        CHECK(result.code == regions::Code::Invalid);
        CHECK(result.param == QStringLiteral("shape.max.x"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("The pointer gate owns a consumed gesture, including its click and double click")
{
    using Event = vc3d::PointerEvent;
    CVolumeViewerView view;
    view.resize(200, 160);
    view.setScrollPanDisabled(true);
    view.show();
    (void)QTest::qWaitForWindowExposed(&view);

    vc3d::PointerGate gate;
    std::vector<Event::Kind> seen;
    bool accept = true;
    vc3d::PointerOverride override = [&](const Event& event) {
        seen.push_back(event.kind);
        if (event.kind == Event::Kind::Cancel || event.kind == Event::Kind::Release) {
            return true;
        }
        return accept;
    };
    int downstreamPress = 0;
    int downstreamClick = 0;
    int downstreamRelease = 0;
    int downstreamDouble = 0;

    const auto event = [](Event::Kind kind, QPointF scene, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
        Event e;
        e.kind = kind;
        e.scenePos = scene;
        e.button = button;
        e.buttons = button;
        e.modifiers = modifiers;
        return e;
    };
    // Wired the way CChunkedVolumeViewer wires its view.
    QObject::connect(&view, &CVolumeViewerView::sendMousePress, [&](QPointF scene, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
        if (gate.offerPress(override, event(Event::Kind::Press, scene, button, mods))) {
            return;
        }
        ++downstreamPress;
    });
    QObject::connect(&view, &CVolumeViewerView::sendMouseMove, [&](QPointF scene, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
        Event e = event(Event::Kind::Move, scene, Qt::NoButton, mods);
        e.buttons = buttons;
        (void)gate.offerMove(override, e);
    });
    QObject::connect(&view, &CVolumeViewerView::sendVolumeClicked, [&](QPointF, Qt::MouseButton, Qt::KeyboardModifiers) {
        if (gate.swallowClick()) {
            return;
        }
        ++downstreamClick;
    });
    QObject::connect(&view, &CVolumeViewerView::sendMouseRelease, [&](QPointF scene, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
        if (gate.offerRelease(override, event(Event::Kind::Release, scene, button, mods))) {
            return;
        }
        ++downstreamRelease;
    });
    QObject::connect(&view, &CVolumeViewerView::sendMouseDoubleClick, [&](QPointF scene, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
        if (gate.offerPress(override, event(Event::Kind::DoubleClick, scene, button, mods)) || gate.owned()) {
            return;
        }
        ++downstreamDouble;
    });
    QObject::connect(&view, &CVolumeViewerView::sendKeyPress, [&](int key, Qt::KeyboardModifiers) {
        if (key == Qt::Key_Escape) {
            (void)gate.offerCancel(override);
        }
    });

    const QPoint pos(80, 70);

    // Accepting override: the click never reaches selection or fibers.
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    REQUIRE(seen.size() >= 2);
    CHECK(seen.front() == Event::Kind::Press);
    CHECK(seen.back() == Event::Kind::Release);
    CHECK(downstreamPress == 0);
    CHECK(downstreamClick == 0);
    CHECK(downstreamRelease == 0);
    CHECK_FALSE(gate.owned());

    // Control: a declining override lets the same click through, so the check
    // above can fail.
    accept = false;
    seen.clear();
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    CHECK(downstreamPress == 1);
    CHECK(downstreamClick == 1);
    CHECK(downstreamRelease == 1);
    CHECK(std::find(seen.begin(), seen.end(), Event::Kind::Release) == seen.end());

    // Modified presses are never offered.
    accept = true;
    seen.clear();
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, pos);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, pos);
    CHECK(seen.empty());
    CHECK(downstreamPress == 2);
    CHECK(downstreamClick == 2);

    // A full double click with an accepting override: nothing downstream. Qt
    // delivers a quick double click as press, release, double click, release
    // (QTest::mouseDClick on a widget sends only the double-click event).
    const int pressesBefore = downstreamPress;
    const int clicksBefore = downstreamClick;
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    CHECK(gate.owned());
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    CHECK(downstreamPress == pressesBefore);
    CHECK(downstreamClick == clicksBefore);
    CHECK(downstreamDouble == 0);
    CHECK_FALSE(gate.owned());

    // Esc while owned cancels and swallows the rest of the gesture.
    seen.clear();
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    REQUIRE(gate.owned());
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &esc);
    CHECK_FALSE(gate.owned());
    CHECK(std::find(seen.begin(), seen.end(), Event::Kind::Cancel) != seen.end());
    const int releasesBefore = downstreamRelease;
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    CHECK(downstreamClick == clicksBefore);
    CHECK(downstreamRelease == releasesBefore);
    CHECK(seen.back() == Event::Kind::Cancel);

    // The next gesture is offered normally again.
    seen.clear();
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, pos);
    REQUIRE(seen.size() >= 2);
    CHECK(seen.back() == Event::Kind::Release);
}

TEST_CASE("A canceled gesture whose release is lost does not keep swallowing input")
{
    using Event = vc3d::PointerEvent;
    vc3d::PointerGate gate;
    int offered = 0;
    vc3d::PointerOverride override = [&](const Event&) {
        ++offered;
        return true;
    };
    Event press;
    press.kind = Event::Kind::Press;
    press.button = Qt::LeftButton;
    press.buttons = Qt::LeftButton;
    Event move;
    move.kind = Event::Kind::Move;

    SUBCASE("a move without the left button ends it")
    {
        REQUIRE(gate.offerPress(override, press));
        CHECK(gate.offerCancel(override));
        move.buttons = Qt::LeftButton;
        offered = 0;
        CHECK(gate.offerMove(override, move));
        CHECK(offered == 0);
        CHECK(gate.swallowClick());
        // The release went to a modal dialog; the next move has no button.
        move.buttons = Qt::NoButton;
        CHECK(gate.offerMove(override, move));
        CHECK(offered == 1);
        CHECK_FALSE(gate.swallowClick());
    }
    SUBCASE("a press with no override installed ends it")
    {
        REQUIRE(gate.offerPress(override, press));
        CHECK(gate.offerCancel(override));
        CHECK(gate.swallowClick());
        CHECK_FALSE(gate.offerPress({}, press));
        CHECK_FALSE(gate.swallowClick());
    }
    SUBCASE("control: while the button stays down the rest is swallowed")
    {
        REQUIRE(gate.offerPress(override, press));
        CHECK(gate.offerCancel(override));
        move.buttons = Qt::LeftButton;
        CHECK(gate.offerMove({}, move));
        CHECK(gate.swallowClick());
    }
}

TEST_CASE("Region tools install only on standard-role slice viewers")
{
    CHECK(regions::shouldInstallTools("standard", "xy plane"));
    CHECK(regions::shouldInstallTools("standard", "seg xz"));
    CHECK(regions::shouldInstallTools("standard", "seg yz"));
    CHECK_FALSE(regions::shouldInstallTools("standard", "segmentation"));
    CHECK_FALSE(regions::shouldInstallTools("annotation", "xy plane"));
    CHECK_FALSE(regions::shouldInstallTools("", "seg xz"));
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    return doctest::detail::runAll();
}
