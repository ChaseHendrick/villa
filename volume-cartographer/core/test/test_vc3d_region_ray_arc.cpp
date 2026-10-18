// Ray arc regions: containment on straight and tilted umbilici, the XY radius,
// conservative block classification against brute force, exact outlines on
// horizontal and vertical planes, statistics and the export mask against a
// brute-force pass, the file format, and the shared annotation umbilicus
// resolution.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "AnnotationUmbilicus.hpp"
#include "RegionExport.hpp"
#include "RegionModel.hpp"
#include "RegionStats.hpp"

#include "vc/core/types/Array3D.hpp"
#include "vc/core/types/Volume.hpp"
#include "vc/core/types/VolumePkg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace regions = vc3d::regions;

namespace
{

fs::path tempRoot(const std::string& tag)
{
    std::mt19937_64 rng(std::random_device{}());
    auto path = fs::temp_directory_path() / ("vc_region_arc_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(path);
    return path;
}

struct TestAutosaveRoot {
    TestAutosaveRoot() : previous(VolumePkg::autosaveRoot()), root(tempRoot("autosave")) { VolumePkg::setAutosaveRoot(root); }
    ~TestAutosaveRoot()
    {
        VolumePkg::setAutosaveRoot(previous);
        fs::remove_all(root);
    }
    fs::path previous;
    fs::path root;
};

TestAutosaveRoot testAutosaveRoot;

regions::Frame frame(long long z, long long y, long long x)
{
    regions::Frame f;
    f.baseShapeZyx = {z, y, x};
    f.voxelSizeUm = 7.91;
    return f;
}

regions::RayArc arcWith(std::vector<cv::Vec3d> points, double start, double sweep, double rmin, double rmax, int z0, int z1)
{
    regions::RayArc arc;
    arc.thetaStartDeg = start;
    arc.thetaSweepDeg = sweep;
    arc.radiusMin = rmin;
    arc.radiusMax = rmax;
    arc.zMin = z0;
    arc.zMax = z1;
    arc.umbilicus.controlPoints = std::move(points);
    arc.umbilicus.sourcePath = "umbilicus.json";
    return arc;
}

regions::CompiledArc compile(const regions::RayArc& arc, const regions::Frame& f)
{
    std::string error;
    auto compiled = regions::compileArc(arc, f, &error);
    REQUIRE_MESSAGE(compiled, error);
    return *compiled;
}

// Point at angle t (degrees from +x toward +y) and XY radius r around (cx, cy).
cv::Vec3d polar(double cx, double cy, double r, double t, double z)
{
    const double a = t * 3.14159265358979323846 / 180.0;
    return {cx + r * std::cos(a), cy + r * std::sin(a), z};
}

}  // namespace

TEST_CASE("Ray arc containment follows the written rule on straight and tilted umbilici")
{
    const regions::Frame f = frame(100, 200, 200);

    SUBCASE("straight umbilicus, a sector across the seam")
    {
        const auto arc = arcWith({{100, 100, 0}, {100, 100, 99}}, 350.0, 20.0, 10.0, 30.0, 20, 60);
        const auto c = compile(arc, f);
        CHECK(regions::contains(arc, c, polar(100, 100, 20, 0, 40)));
        CHECK(regions::contains(arc, c, polar(100, 100, 20, 355, 40)));
        CHECK(regions::contains(arc, c, polar(100, 100, 20, 9.5, 40)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 20, 11, 40)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 20, 348, 40)));
        // Inclusive radii and z.
        CHECK(regions::contains(arc, c, polar(100, 100, 10, 0, 20)));
        CHECK(regions::contains(arc, c, polar(100, 100, 30, 0, 60)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 9.99, 0, 40)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 30.01, 0, 40)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 20, 0, 19)));
        CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 20, 0, 61)));
    }
    SUBCASE("a full sweep is an annulus")
    {
        const auto arc = arcWith({{100, 100, 0}}, 0.0, 360.0, 5.0, 15.0, 0, 99);
        const auto c = compile(arc, f);
        for (double t = 0.0; t < 360.0; t += 7.5) {
            CHECK(regions::contains(arc, c, polar(100, 100, 10, t, 50)));
            CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 4, t, 50)));
        }
    }
    SUBCASE("theta increases from the seam toward +y, for every seam")
    {
        for (const auto& [seam, base] : std::map<std::string, double>{{"+x", 0.0}, {"+y", 90.0}, {"-x", 180.0}, {"-y", 270.0}}) {
            auto arc = arcWith({{100, 100, 0}}, 0.0, 90.0, 5.0, 15.0, 0, 99);
            arc.umbilicus.seam = seam;
            const auto c = compile(arc, f);
            CHECK_MESSAGE(regions::thetaDeg(c, polar(100, 100, 10, base + 45.0, 50)) == doctest::Approx(45.0), seam);
            CHECK(regions::contains(arc, c, polar(100, 100, 10, base + 45.0, 50)));
            CHECK_FALSE(regions::contains(arc, c, polar(100, 100, 10, base - 45.0, 50)));
        }
    }
    SUBCASE("a tilted umbilicus moves the center slice by slice")
    {
        // Center x drifts from 80 at z=0 to 120 at z=99.
        const auto arc = arcWith({{80, 100, 0}, {120, 100, 99}}, 0.0, 360.0, 0.0, 5.0, 0, 99);
        const auto c = compile(arc, f);
        const cv::Vec2d top = c.centerAt(99);
        CHECK(top[0] == doctest::Approx(120.0));
        CHECK(regions::contains(arc, c, {120, 100, 99}));
        CHECK_FALSE(regions::contains(arc, c, {80, 100, 99}));
        CHECK(regions::contains(arc, c, {80, 100, 0}));
    }
}

TEST_CASE("The arc radius is the XY distance to the center of the point's slice")
{
    const regions::Frame f = frame(100, 200, 200);
    const auto arc = arcWith({{80, 100, 0}, {120, 100, 99}}, 0.0, 360.0, 0.0, 50.0, 0, 99);
    const auto c = compile(arc, f);
    // 0.4 slices off slice 50 rounds to 50, same center, same radius.
    const cv::Vec2d center = c.centerAt(50);
    CHECK(regions::radiusXY(c, {center[0] + 7, center[1], 50.4}) == doctest::Approx(7.0));
    CHECK(regions::radiusXY(c, {center[0] + 7, center[1], 50.0}) == doctest::Approx(7.0));
    // A 3D distance would grow with the z offset; the XY radius does not.
    CHECK(regions::radiusXY(c, {center[0], center[1] + 3, 49.6}) == doctest::Approx(3.0));
    // Below slice 0: the clamped center and the XY distance only.
    const cv::Vec2d bottom = c.centerAt(0);
    CHECK(regions::radiusXY(c, {bottom[0] + 4, bottom[1], -25.0}) == doctest::Approx(4.0));
}

TEST_CASE("Block classification never says Inside or Outside wrongly")
{
    const regions::Frame f = frame(64, 160, 160);
    std::mt19937_64 rng(20261018);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    int partial = 0;
    int decided = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        const double start = trial % 5 == 0 ? 350.0 : u01(rng) * 360.0;
        const double sweep = trial % 7 == 0 ? 360.0 : 5.0 + u01(rng) * 200.0;
        const double rmin = u01(rng) * 30.0;
        const double rmax = rmin + 1.0 + u01(rng) * 40.0;
        const auto arc = arcWith({{70, 80, 0}, {90, 75, 63}}, start, sweep, rmin, rmax, 5 + trial % 10, 40 + trial % 20);
        const auto c = compile(arc, f);
        const double size = 1.0 + u01(rng) * 24.0;
        cv::Vec3d lo(30 + u01(rng) * 90, 30 + u01(rng) * 90, u01(rng) * 60);
        if (trial % 9 == 0) {
            // A block containing the center.
            const cv::Vec2d center = c.centerAt(lo[2]);
            lo[0] = center[0] - size / 2;
            lo[1] = center[1] - size / 2;
        }
        const cv::Vec3d hi = lo + cv::Vec3d(size, size, std::min(size, 8.0));
        const regions::Coverage coverage = regions::classify(arc, c, lo, hi);
        if (coverage == regions::Coverage::Partial) {
            ++partial;
            continue;
        }
        ++decided;
        bool anyIn = false;
        bool anyOut = false;
        for (double z = lo[2]; z <= hi[2] + 1e-9; z += std::max(0.5, (hi[2] - lo[2]) / 6)) {
            for (double y = lo[1]; y <= hi[1] + 1e-9; y += (hi[1] - lo[1]) / 12) {
                for (double x = lo[0]; x <= hi[0] + 1e-9; x += (hi[0] - lo[0]) / 12) {
                    (regions::contains(arc, c, {x, y, z}) ? anyIn : anyOut) = true;
                }
            }
        }
        if (coverage == regions::Coverage::Inside) {
            CHECK_MESSAGE(!anyOut, "trial " << trial << " classified Inside with a point outside");
        } else {
            CHECK_MESSAGE(!anyIn, "trial " << trial << " classified Outside with a point inside");
        }
    }
    // The check can fail only if some blocks are decided.
    CHECK(decided > 500);
    CHECK(partial > 100);
}

TEST_CASE("Arc outlines are exact on horizontal planes and match row membership on vertical planes")
{
    const regions::Frame f = frame(64, 200, 200);

    SUBCASE("horizontal outline points lie on the two radii")
    {
        const auto arc = arcWith({{100, 100, 0}}, 30.0, 100.0, 20.0, 45.0, 0, 63);
        const auto c = compile(arc, f);
        const auto paths = regions::arcOutlineHorizontal(arc, c, 10.0, 0.5);
        REQUIRE(paths.size() == 1);
        int onOuter = 0;
        int onInner = 0;
        for (const auto& p : paths.front()) {
            const double r = regions::radiusXY(c, p);
            const bool outer = std::abs(r - 45.0) < 1e-9;
            const bool inner = std::abs(r - 20.0) < 1e-9;
            CHECK((outer || inner));
            onOuter += outer ? 1 : 0;
            onInner += inner ? 1 : 0;
        }
        CHECK(onOuter > 100);
        CHECK(onInner > 100);
        CHECK(regions::arcOutlineHorizontal(arc, c, 70.0, 0.5).empty());
        const auto full = arcWith({{100, 100, 0}}, 0.0, 360.0, 20.0, 45.0, 0, 63);
        CHECK(regions::arcOutlineHorizontal(full, compile(full, f), 10.0, 0.5).size() == 2);
    }
    SUBCASE("vertical rows equal brute-force membership for a drifting umbilicus")
    {
        const auto arc = arcWith({{90, 100, 0}, {110, 104, 63}}, 300.0, 120.0, 10.0, 40.0, 4, 60);
        const auto c = compile(arc, f);
        const cv::Vec2d origin(60.0, 103.5);
        const cv::Vec2d h(1.0, 0.0);
        for (int m = 0; m < 64; ++m) {
            const auto intervals = regions::arcRowIntervals(arc, c, origin, h, m);
            for (double u = 0.25; u < 100.0; u += 0.5) {
                const cv::Vec3d p(origin[0] + u, origin[1], m);
                bool inInterval = false;
                for (const auto& [a, b] : intervals) {
                    inInterval = inInterval || (u >= a && u <= b);
                }
                CHECK_MESSAGE(inInterval == regions::contains(arc, c, p), "slice " << m << " u " << u);
            }
        }
        const auto segments = regions::arcOutlineVertical(arc, c, origin, h, 0, 63, 1);
        CHECK_FALSE(segments.empty());
        for (const auto& segment : segments) {
            CHECK(segment[0][2] >= 3.5);
            CHECK(segment[1][2] <= 60.5);
        }
    }
}

TEST_CASE("Arc handles and two-click creation")
{
    const regions::Frame f = frame(64, 200, 200);
    const auto arc = arcWith({{100, 100, 0}}, 30.0, 60.0, 20.0, 40.0, 10, 30);
    const auto c = compile(arc, f);

    const auto made = regions::arcFromClicks(c, polar(100, 100, 25, 10, 20), polar(100, 100, 50, 70, 20), true, 20.0, 16, 63);
    REQUIRE(made);
    CHECK(made->thetaStartDeg == doctest::Approx(10.0).epsilon(1e-4));
    CHECK(made->thetaSweepDeg == doctest::Approx(60.0).epsilon(1e-4));
    CHECK(made->radiusMin == doctest::Approx(25.0));
    CHECK(made->radiusMax == doctest::Approx(50.0));
    CHECK(made->zMin == 12);
    CHECK(made->zMax == 27);
    const auto back = regions::arcFromClicks(c, polar(100, 100, 25, 10, 20), polar(100, 100, 50, 70, 20), false, 20.0, 16, 63);
    REQUIRE(back);
    CHECK(back->thetaStartDeg == doctest::Approx(70.0).epsilon(1e-4));
    CHECK(back->thetaSweepDeg == doctest::Approx(300.0).epsilon(1e-4));
    CHECK_FALSE(regions::arcFromClicks(c, polar(100, 100, 25, 10, 20), polar(100, 100, 25.5, 70, 20), true, 20.0, 16, 63));

    const auto handles = regions::arcHandles(arc, c, 20.0);
    REQUIRE(handles.size() == 4);
    const auto outward = regions::dragArcHandle(arc, c, regions::ArcHandleKind::RadiusMax, polar(100, 100, 55, 60, 20));
    CHECK(outward.radiusMax == doctest::Approx(55.0));
    CHECK(outward.radiusMin == doctest::Approx(20.0));
    // Dragging the outer radius inside the inner one swaps them.
    const auto swapped = regions::dragArcHandle(arc, c, regions::ArcHandleKind::RadiusMax, polar(100, 100, 5, 60, 20));
    CHECK(swapped.radiusMin == doctest::Approx(5.0));
    CHECK(swapped.radiusMax == doctest::Approx(20.0));
    const auto end = regions::dragArcHandle(arc, c, regions::ArcHandleKind::ThetaEnd, polar(100, 100, 30, 120, 20));
    CHECK(end.thetaStartDeg == doctest::Approx(30.0));
    CHECK(end.thetaSweepDeg == doctest::Approx(90.0).epsilon(1e-4));
    const auto start = regions::dragArcHandle(arc, c, regions::ArcHandleKind::ThetaStart, polar(100, 100, 30, 0, 20));
    CHECK(start.thetaStartDeg == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(start.thetaSweepDeg == doctest::Approx(90.0).epsilon(1e-4));
}

TEST_CASE("Ray arc files round trip and report bad fields")
{
    regions::Region region;
    region.id = "arc-1";
    region.name = "arc";
    region.revision = 2;
    region.frame = frame(64, 200, 200);
    auto arc = arcWith({{100.5, 99.25, 0.0}, {101.0, 99.0, 63.0}}, 30.0, 25.0, 9.5, 14.0, 6, 40);
    arc.umbilicus.scale = 4.0;
    arc.umbilicus.scaleSource = "stamped_dimensions";
    arc.umbilicus.seam = "-y";
    region.shape = arc;
    const std::string text = regions::toJson(region).dump(2);
    const auto parsed = regions::parseRegion(regions::Json::parse(text));
    REQUIRE_MESSAGE(parsed.loaded, parsed.errorPath << ": " << parsed.error);
    CHECK_FALSE(parsed.loaded->readOnly);
    REQUIRE(regions::arcOf(parsed.loaded->region));
    CHECK(*regions::arcOf(parsed.loaded->region) == arc);
    CHECK(regions::toJson(parsed.loaded->region).dump(2) == text);

    const auto broken = [&](const std::string& path, const regions::Json& value) {
        auto doc = regions::Json::parse(text);
        regions::Json* node = &doc;
        std::string rest = path;
        std::size_t dot = 0;
        while ((dot = rest.find('.')) != std::string::npos) {
            node = &(*node)[rest.substr(0, dot)];
            rest = rest.substr(dot + 1);
        }
        (*node)[rest] = value;
        return regions::parseRegion(doc);
    };
    CHECK(broken("shape.theta_start_deg", 360.0).errorPath == "shape.theta_start_deg");
    CHECK(broken("shape.theta_sweep_deg", 0.0).errorPath == "shape.theta_sweep_deg");
    CHECK(broken("shape.radius_max", 10.0).errorPath == "shape.radius_max");
    CHECK(broken("shape.z_max", 64).errorPath == "shape.z_max");
    CHECK(broken("shape.umbilicus.seam", "x").errorPath == "shape.umbilicus.seam");
    CHECK(broken("shape.umbilicus.scale_source", "guess").errorPath == "shape.umbilicus.scale_source");
    CHECK(broken("shape.umbilicus.control_points", regions::Json::array()).errorPath == "shape.umbilicus.control_points");
    const auto extra = broken("shape.umbilicus.extra", 1);
    REQUIRE(extra.loaded);
    CHECK(extra.loaded->readOnly);
}

namespace
{

fs::path makeVolume8(const fs::path& root, std::size_t levels)
{
    Volume::ZarrCreateOptions opts;
    opts.shapeZYX = {40, 70, 66};
    opts.chunkShapeZYX = {16, 16, 16};
    opts.numLevels = levels;
    opts.dtype = vc::render::ChunkDtype::UInt8;
    opts.fillValue = 0.0;
    opts.voxelSize = 2.0;
    opts.name = "arc-test";
    opts.uuid = "arc-test";
    opts.overwriteExisting = true;
    opts.compressor = "none";
    const fs::path path = root / "vol8.zarr";
    auto volume = Volume::New(path, opts);
    Array3D<std::uint8_t> data({40, 70, 66});
    for (std::size_t z = 0; z < 40; ++z) {
        for (std::size_t y = 0; y < 70; ++y) {
            for (std::size_t x = 0; x < 66; ++x) {
                data(z, y, x) = static_cast<std::uint8_t>((z * 31 + y * 7 + x * 13 + x * y) % 256);
            }
        }
    }
    volume->writeZYX(data, {0, 0, 0}, 0);
    return path;
}

}  // namespace

TEST_CASE("Ray arc statistics and the export mask equal a brute-force pass")
{
    const fs::path root = tempRoot("stats");
    const fs::path path = makeVolume8(root, 2);
    auto volume = Volume::New(path);

    regions::Region region;
    region.id = "arc-stats";
    region.name = "arc";
    region.revision = 1;
    region.frame = frame(40, 70, 66);
    region.shape = arcWith({{30, 34, 0}, {36, 38, 39}}, 340.0, 110.0, 6.0, 22.0, 3, 33);
    std::string error;
    const auto spec = regions::shapeSpec(region, &error);
    REQUIRE_MESSAGE(spec, error);
    const regions::RayArc& arc = *regions::arcOf(region);

    for (const int level : {0, 1}) {
        CAPTURE(level);
        regions::StatsRequest request;
        request.level = level;
        request.threshold = 100.0;
        const auto stats = regions::computeStats(*volume, "vol", *spec, regions::FrameMap{0}, request, {}, &error);
        REQUIRE_MESSAGE(stats, error);
        const auto shape = volume->shape(level);
        Array3D<std::uint8_t> all({static_cast<std::size_t>(shape[0]), static_cast<std::size_t>(shape[1]), static_cast<std::size_t>(shape[2])});
        REQUIRE(volume->readZYX(all, {0, 0, 0}, level));
        std::vector<std::uint32_t> values;
        const double s = std::ldexp(1.0, level);
        for (int z = 0; z < shape[0]; ++z) {
            for (int y = 0; y < shape[1]; ++y) {
                for (int x = 0; x < shape[2]; ++x) {
                    if (regions::contains(arc, *spec->arc, {x * s, y * s, z * s})) {
                        values.push_back(all(static_cast<std::size_t>(z), static_cast<std::size_t>(y), static_cast<std::size_t>(x)));
                    }
                }
            }
        }
        REQUIRE(!values.empty());
        std::sort(values.begin(), values.end());
        std::uint64_t sum = 0;
        std::uint64_t above = 0;
        std::array<std::uint64_t, 256> histogram{};
        for (const auto v : values) {
            sum += v;
            above += v > 100 ? 1 : 0;
            ++histogram[v];
        }
        CHECK(stats->count == values.size());
        CHECK(stats->min == values.front());
        CHECK(stats->max == values.back());
        CHECK(stats->sum == sum);
        CHECK(stats->countAbove == above);
        CHECK(stats->histogram == histogram);
        if (level == 0) {
            CHECK(stats->chunksSkipped > 0);
        }
    }

    SUBCASE("the export writes the bounding range and a 0/255 mask of the arc")
    {
        regions::ExportRequest request;
        request.outDir = root / "crop";
        request.level = 0;
        request.created = "2026-10-18T14:10:00Z";
        const auto result = regions::exportCrop(*volume, "vol", path.string(), region, regions::FrameMap{0}, request, {});
        REQUIRE_MESSAGE(result.ok(), result.error);
        auto mask = Volume::New(result.dir / "mask.zarr");
        const auto origin = result.manifest["crop"]["index_origin_zyx"];
        const auto cropShape = mask->shape(0);
        Array3D<std::uint8_t> m(
            {static_cast<std::size_t>(cropShape[0]), static_cast<std::size_t>(cropShape[1]), static_cast<std::size_t>(cropShape[2])});
        REQUIRE(mask->readZYX(m, {0, 0, 0}, 0));
        std::uint64_t inside = 0;
        for (int z = 0; z < cropShape[0]; ++z) {
            for (int y = 0; y < cropShape[1]; ++y) {
                for (int x = 0; x < cropShape[2]; ++x) {
                    const cv::Vec3d p(x + origin[2].get<int>(), y + origin[1].get<int>(), z + origin[0].get<int>());
                    const std::uint8_t expected = regions::contains(arc, *spec->arc, p) ? 255 : 0;
                    CHECK(m(static_cast<std::size_t>(z), static_cast<std::size_t>(y), static_cast<std::size_t>(x)) == expected);
                    inside += expected ? 1 : 0;
                }
            }
        }
        CHECK(inside > 0);
        CHECK(result.manifest["crop"]["files"].size() == 2);
    }
    fs::remove_all(root);
}

namespace
{

void writeUmbilicusFile(const fs::path& path, const std::string& metadata, const std::vector<std::array<double, 3>>& points)
{
    std::ofstream out(path);
    out << R"({"metadata": {)" << metadata << R"(}, "control_points": [)";
    for (std::size_t i = 0; i < points.size(); ++i) {
        out << (i ? "," : "") << R"({"x": )" << points[i][0] << R"(, "y": )" << points[i][1] << R"(, "z": )" << points[i][2] << "}";
    }
    out << "]}";
}

vc3d::annotation::AnnotationFrame annotationFrame(double x, double y, double z)
{
    vc3d::annotation::AnnotationFrame f;
    f.factor = 1.0;
    f.extentXyz = {x, y, z};
    f.voxelSizeUm = 2.4;
    return f;
}

}  // namespace

TEST_CASE("The shared annotation umbilicus resolution frames stamped and inferred files")
{
    using vc::core::util::UmbilicusLoadAction;
    using vc::core::util::UmbilicusScaleSource;
    const fs::path root = tempRoot("umbilicus");
    auto pkg = VolumePkg::newEmpty();
    pkg->save(root / "project.json");
    const auto target = annotationFrame(4000, 3600, 8000);

    SUBCASE("a file stamped with x4 dimensions applies at x4")
    {
        writeUmbilicusFile(root / "umbilicus.json", R"("volume_width": 1000, "volume_height": 900, "volume_slices": 2000)", {{500, 450, 0}, {510, 440, 1999}});
        const auto resolved = vc3d::annotation::resolveAnnotationUmbilicus(*pkg, target);
        CHECK(resolved.error.empty());
        REQUIRE(resolved.scale);
        CHECK(resolved.scale->source == UmbilicusScaleSource::StampedDimensions);
        CHECK(resolved.scale->factor == doctest::Approx(4.0));
        CHECK(resolved.action == UmbilicusLoadAction::Apply);
        const auto points = vc3d::annotation::framedControlPoints(resolved);
        REQUIRE(points.size() == 2);
        CHECK(points[0][0] == doctest::Approx(2000.0));
    }
    SUBCASE("an unstamped file filling a quarter grid is inferred at x4")
    {
        writeUmbilicusFile(root / "umbilicus.json", R"("total_points": 2)", {{500, 450, 0}, {510, 440, 1999}});
        const auto resolved = vc3d::annotation::resolveAnnotationUmbilicus(*pkg, target);
        REQUIRE(resolved.scale);
        CHECK(resolved.scale->source == UmbilicusScaleSource::InferredFromGrid);
        CHECK(resolved.scale->factor == doctest::Approx(4.0));
        CHECK(resolved.action == UmbilicusLoadAction::Apply);
    }
    SUBCASE("an unstamped file that defeats inference is left to legacy readers, which regions refuse")
    {
        writeUmbilicusFile(root / "umbilicus.json", R"("total_points": 2)", {{500, 450, 10}, {510, 440, 300}});
        const auto resolved = vc3d::annotation::resolveAnnotationUmbilicus(*pkg, target);
        CHECK_FALSE(resolved.scale);
        CHECK(resolved.action == UmbilicusLoadAction::UseLegacy);
        CHECK(vc3d::annotation::framedControlPoints(resolved).empty());
    }
    SUBCASE("a stamp contradicted by the volume it names is refused")
    {
        // The named store exists in the project with a grid the stamp is not a
        // uniform rescale of.
        Volume::ZarrCreateOptions opts;
        opts.shapeZYX = {30, 20, 10};
        opts.chunkShapeZYX = {16, 16, 16};
        opts.voxelSize = 2.4;
        opts.compressor = "none";
        opts.name = "named";
        opts.uuid = "named";
        Volume::New(root / "named.zarr", opts);
        REQUIRE(pkg->addVolumeEntry((root / "named.zarr").string()));
        writeUmbilicusFile(root / "umbilicus.json", R"("volume": "named.zarr", "volume_width": 1000, "volume_height": 900, "volume_slices": 2000)", {{500, 450, 0}, {510, 440, 1999}});
        const auto resolved = vc3d::annotation::resolveAnnotationUmbilicus(*pkg, target);
        REQUIRE(resolved.stampedVolume);
        CHECK(resolved.stampContradiction);
        CHECK(resolved.action == UmbilicusLoadAction::Refuse);
    }
    SUBCASE("no file reports the resolver's error")
    {
        const auto resolved = vc3d::annotation::resolveAnnotationUmbilicus(*pkg, target);
        CHECK(resolved.path.empty());
        CHECK_FALSE(resolved.error.empty());
    }
    fs::remove_all(root);
}
