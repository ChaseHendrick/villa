// Region statistics and crop export on local zarr volumes: exact numbers
// against a brute-force pass, thread-count independence, cancel, fill-value
// chunks, the level index-range rule, 128-bit moments, inverted-CDF
// percentiles, and the export's empty-folder, partial-folder and manifest
// rules.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "RegionExport.hpp"
#include "RegionModel.hpp"
#include "RegionStats.hpp"

#include "vc/core/types/Array3D.hpp"
#include "vc/core/types/Volume.hpp"
#include "vc/core/types/VolumePkg.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    auto path = fs::temp_directory_path() / ("vc_region_stats_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(path);
    return path;
}

std::uint16_t pattern16(std::size_t z, std::size_t y, std::size_t x)
{
    return static_cast<std::uint16_t>((z * 7919u + y * 104729u + x * 1299709u + z * y * x * 31u) % 65536u);
}

std::uint8_t pattern8(std::size_t z, std::size_t y, std::size_t x)
{
    return static_cast<std::uint8_t>((z * 13u + y * 7u + x * 3u + z * y * x) % 256u);
}

Volume::ZarrCreateOptions options(std::array<std::size_t, 3> shapeZyx, std::size_t chunk, std::size_t levels, vc::render::ChunkDtype dtype)
{
    Volume::ZarrCreateOptions opts;
    opts.shapeZYX = shapeZyx;
    opts.chunkShapeZYX = {chunk, chunk, chunk};
    opts.numLevels = levels;
    opts.dtype = dtype;
    opts.fillValue = 0.0;
    opts.voxelSize = 2.0;
    opts.name = "region-stats-test";
    opts.uuid = "region-stats-test";
    opts.overwriteExisting = true;
    opts.compressor = "none";
    return opts;
}

// A uint16 volume of shape 70 x 45 x 33 with 16-cube chunks: no axis is a
// multiple of the chunk size, so a range touching a far edge ends inside a
// partial chunk.
fs::path makeVolume16(const fs::path& root, std::size_t levels = 2)
{
    const fs::path path = root / "vol16.zarr";
    auto volume = Volume::New(path, options({70, 45, 33}, 16, levels, vc::render::ChunkDtype::UInt16));
    Array3D<std::uint16_t> data({70, 45, 33});
    for (std::size_t z = 0; z < 70; ++z) {
        for (std::size_t y = 0; y < 45; ++y) {
            for (std::size_t x = 0; x < 33; ++x) {
                data(z, y, x) = pattern16(z, y, x);
            }
        }
    }
    volume->writeZYX(data, {0, 0, 0}, 0);
    return path;
}

regions::Box box(int x0, int y0, int z0, int x1, int y1, int z1)
{
    regions::Box b;
    b.min = {x0, y0, z0};
    b.max = {x1, y1, z1};
    return b;
}

struct Brute {
    std::vector<std::uint32_t> values;
    std::uint64_t sum = 0;
    unsigned __int128 sumSquares = 0;
};

Brute bruteForce(Volume& volume, int level, const regions::LevelRange& range)
{
    Brute out;
    const std::array<std::size_t, 3> shape = {
        static_cast<std::size_t>(range.maxZyx[0] - range.minZyx[0] + 1),
        static_cast<std::size_t>(range.maxZyx[1] - range.minZyx[1] + 1),
        static_cast<std::size_t>(range.maxZyx[2] - range.minZyx[2] + 1),
    };
    const bool wide = volume.dtype() == vc::render::ChunkDtype::UInt16;
    if (wide) {
        Array3D<std::uint16_t> block(shape);
        REQUIRE(volume.readZYX(block, {range.minZyx[0], range.minZyx[1], range.minZyx[2]}, level));
        out.values.assign(block.data_.begin(), block.data_.end());
    } else {
        Array3D<std::uint8_t> block(shape);
        REQUIRE(volume.readZYX(block, {range.minZyx[0], range.minZyx[1], range.minZyx[2]}, level));
        out.values.assign(block.data_.begin(), block.data_.end());
    }
    for (const auto v : out.values) {
        out.sum += v;
        out.sumSquares += static_cast<unsigned __int128>(v) * v;
    }
    std::sort(out.values.begin(), out.values.end());
    return out;
}

std::uint32_t invertedCdf(const std::vector<std::uint32_t>& sorted, double percent)
{
    if (percent <= 0.0) {
        return sorted.front();
    }
    const auto n = static_cast<long double>(sorted.size());
    const auto rank = static_cast<std::size_t>(std::ceil(static_cast<long double>(percent) / 100.0L * n - 1e-12L));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

void checkAgainstBruteForce(Volume& volume, const regions::Box& b, const regions::FrameMap& map, int level)
{
    regions::StatsRequest request;
    request.level = level;
    request.threshold = 30000.0;
    std::string error;
    const auto stats = regions::computeStats(volume, "vol", b, map, request, {}, &error);
    REQUIRE_MESSAGE(stats, error);
    const auto brute = bruteForce(volume, level, stats->range);
    REQUIRE(!brute.values.empty());
    CHECK(stats->count == brute.values.size());
    CHECK(stats->min == brute.values.front());
    CHECK(stats->max == brute.values.back());
    CHECK(stats->sum == brute.sum);
    CHECK(regions::toDecimal(stats->sumSquares) == regions::toDecimal(brute.sumSquares));
    std::array<std::uint64_t, 256> histogram{};
    std::uint64_t above = 0;
    for (const auto v : brute.values) {
        ++histogram[v >> 8];
        above += v > 30000 ? 1 : 0;
    }
    CHECK(stats->histogram == histogram);
    CHECK(stats->binWidth == 256);
    CHECK(stats->countAbove == above);
    REQUIRE(stats->percentiles.size() == request.percentiles.size());
    for (const auto& [p, value] : stats->percentiles) {
        CHECK_MESSAGE(value == invertedCdf(brute.values, p), "percentile " << p);
    }
    const double mean = static_cast<double>(brute.sum) / static_cast<double>(brute.values.size());
    CHECK(stats->mean == doctest::Approx(mean).epsilon(1e-12));
    CHECK_FALSE(stats->canceled);
}

}  // namespace

TEST_CASE("Box statistics equal a brute-force pass at levels 0 and 1, including far-edge chunks")
{
    const fs::path root = tempRoot("brute");
    auto volume = Volume::New(makeVolume16(root));
    const regions::FrameMap identity{0};
    const auto shape0 = volume->shape(0);  // z y x
    REQUIRE(shape0 == std::array<int, 3>{70, 45, 33});

    SUBCASE("interior box")
    {
        checkAgainstBruteForce(*volume, box(3, 5, 7, 20, 30, 50), identity, 0);
    }
    SUBCASE("boxes touching each far edge")
    {
        checkAgainstBruteForce(*volume, box(10, 2, 3, 32, 20, 30), identity, 0);  // x edge
        checkAgainstBruteForce(*volume, box(1, 30, 3, 20, 44, 30), identity, 0);  // y edge
        checkAgainstBruteForce(*volume, box(1, 2, 50, 20, 20, 69), identity, 0);  // z edge
        checkAgainstBruteForce(*volume, box(0, 0, 0, 32, 44, 69), identity, 0);   // whole volume
    }
    SUBCASE("level 1 against its own stored values")
    {
        checkAgainstBruteForce(*volume, box(1, 1, 1, 32, 44, 69), identity, 1);
        checkAgainstBruteForce(*volume, box(5, 9, 13, 17, 21, 41), identity, 1);
    }
    SUBCASE("a region drawn at half resolution maps with k = 2")
    {
        // Region points r are viewer points 2r: the box covers twice as many
        // level-0 voxels per axis.
        const regions::FrameMap twice{1};
        const auto stats = regions::computeStats(*volume, "vol", box(2, 3, 4, 10, 12, 20), twice, {.level = 0}, {}, nullptr);
        REQUIRE(stats);
        CHECK(stats->range.minZyx == cv::Vec3i(8, 6, 4));
        CHECK(stats->range.maxZyx == cv::Vec3i(40, 24, 20));
    }
    fs::remove_all(root);
}

TEST_CASE("Statistics do not depend on the thread count, and a cancel stops early")
{
    const fs::path root = tempRoot("threads");
    auto volume = Volume::New(makeVolume16(root));
    const regions::FrameMap identity{0};
    const regions::Box b = box(0, 0, 0, 32, 44, 69);

    regions::StatsRequest one;
    one.level = 0;
    one.threads = 1;
    regions::StatsRequest four = one;
    four.threads = 4;
    const auto a = regions::computeStats(*volume, "vol", b, identity, one, {}, nullptr);
    const auto c = regions::computeStats(*volume, "vol", b, identity, four, {}, nullptr);
    REQUIRE(a);
    REQUIRE(c);
    CHECK(a->count == c->count);
    CHECK(a->sum == c->sum);
    CHECK(regions::toDecimal(a->sumSquares) == regions::toDecimal(c->sumSquares));
    CHECK(a->histogram == c->histogram);
    CHECK(a->percentiles == c->percentiles);
    CHECK(a->mean == c->mean);
    CHECK(a->stddev == c->stddev);
    CHECK(a->chunksRead == c->chunksRead);

    std::atomic<bool> cancel{false};
    regions::Progress progress;
    progress.cancel = &cancel;
    progress.report = [&cancel](double) { cancel.store(true); };
    const auto canceled = regions::computeStats(*volume, "vol", b, identity, one, progress, nullptr);
    REQUIRE(canceled);
    CHECK(canceled->canceled);
    CHECK(canceled->chunksRead == 1);
    CHECK(canceled->chunksRead < a->chunksRead);
    fs::remove_all(root);
}

TEST_CASE("A chunk absent from the store counts as the fill value")
{
    const fs::path root = tempRoot("fill");
    const fs::path path = root / "fill.zarr";
    {
        auto opts = options({32, 32, 32}, 16, 1, vc::render::ChunkDtype::UInt8);
        opts.fillValue = 7.0;
        auto volume = Volume::New(path, opts);
        Array3D<std::uint8_t> data({32, 32, 32});
        for (std::size_t z = 0; z < 32; ++z) {
            for (std::size_t y = 0; y < 32; ++y) {
                for (std::size_t x = 0; x < 32; ++x) {
                    data(z, y, x) = static_cast<std::uint8_t>(100 + (pattern8(z, y, x) % 100));
                }
            }
        }
        volume->writeZYX(data, {0, 0, 0}, 0);
        REQUIRE(volume->removeChunk(0, {0, 0, 0}));
    }
    auto volume = Volume::New(path);
    const auto stats = regions::computeStats(*volume, "vol", box(0, 0, 0, 31, 31, 31), regions::FrameMap{0}, {.level = 0}, {}, nullptr);
    REQUIRE(stats);
    CHECK(stats->count == 32u * 32u * 32u);
    CHECK(stats->fillChunks == 1);
    CHECK(stats->histogram[7] == 16u * 16u * 16u);
    CHECK(stats->min == 7);
    fs::remove_all(root);
}

TEST_CASE("Level index ranges follow the corner convention for k = 1, 1/2 and 1/4")
{
    const std::array<int, 3> level0Shape = {997, 1003, 1009};  // z y x
    for (const int log2k : {0, -1, -2}) {
        const double k = std::ldexp(1.0, log2k);
        for (int level = 0; level <= 3; ++level) {
            std::array<int, 3> shape{};
            for (int axis = 0; axis < 3; ++axis) {
                shape[axis] = (level0Shape[axis] + (1 << level) - 1) >> level;
            }
            for (int m = 0; m < 6; ++m) {
                const regions::Box b = box(4 * m + 1, 4 * m + 2, 4 * m + 3, 4 * m + 1 + 37 * (m + 1), 4 * m + 6 + 11 * m, 4 * m + 9 + 50 * m);
                const auto range = regions::boxIndexRange(b, regions::FrameMap{log2k}, level, shape);
                for (int axis = 0; axis < 3; ++axis) {
                    const int zyx = 2 - axis;
                    int lo = -1;
                    int hi = -1;
                    for (int i = 0; i < shape[zyx]; ++i) {
                        const double regionPoint = static_cast<double>(i) * std::ldexp(1.0, level) / k;
                        if (regionPoint >= b.min[axis] && regionPoint <= b.max[axis]) {
                            if (lo < 0) {
                                lo = i;
                            }
                            hi = i;
                        }
                    }
                    if (lo < 0) {
                        CHECK(range.empty());
                    } else {
                        CHECK_MESSAGE(range.minZyx[zyx] == lo, "k=" << k << " L=" << level << " axis=" << axis << " m=" << m);
                        CHECK_MESSAGE(range.maxZyx[zyx] == hi, "k=" << k << " L=" << level << " axis=" << axis << " m=" << m);
                    }
                }
            }
        }
    }
}

TEST_CASE("Moments are exact past 2^64 and percentiles are numpy's inverted CDF")
{
    std::vector<std::uint64_t> wide(65536, 0);
    wide[65535] = std::uint64_t{1} << 40;
    wide[3] = 5;
    const auto m = regions::momentsOf(wide);
    CHECK(m.n == (std::uint64_t{1} << 40) + 5);
    CHECK(m.s1 == 65535ull * (std::uint64_t{1} << 40) + 15ull);
    // 65535^2 * 2^40 + 9 * 5, beyond 2^64.
    CHECK(regions::toDecimal(m.s2) == "4722222368781080985645");

    std::vector<std::uint64_t> h(256, 0);
    h[1] = 2;
    h[3] = 2;
    h[7] = 1;  // sorted: 1 1 3 3 7
    CHECK(regions::percentileOf(h, 5, 0.0) == 1);
    CHECK(regions::percentileOf(h, 5, 20.0) == 1);
    CHECK(regions::percentileOf(h, 5, 40.0) == 1);
    CHECK(regions::percentileOf(h, 5, 40.0001) == 3);
    CHECK(regions::percentileOf(h, 5, 50.0) == 3);
    CHECK(regions::percentileOf(h, 5, 80.0) == 3);
    CHECK(regions::percentileOf(h, 5, 80.0001) == 7);
    CHECK(regions::percentileOf(h, 5, 100.0) == 7);
    CHECK(regions::isValidPercentile(99.5));
    CHECK(regions::isValidPercentile(0.0001));
    CHECK_FALSE(regions::isValidPercentile(0.00001));
    CHECK_FALSE(regions::isValidPercentile(100.5));
    CHECK_FALSE(regions::isValidPercentile(-1.0));
}

TEST_CASE("Crop export refuses a non-empty folder, cleans up on cancel and places the crop")
{
    const fs::path root = tempRoot("export");
    auto volume = Volume::New(makeVolume16(root));
    regions::Region region;
    region.id = "6f1c2c1e-4a7b-4f43-9a7e-2b2d9a0e9c11";
    region.name = "crop";
    region.revision = 1;
    region.frame.baseShapeZyx = {35, 23, 17};  // drawn on a half-resolution grid
    region.shape = box(2, 3, 4, 12, 15, 30);
    const regions::FrameMap map{1};  // region point r is viewer point 2r

    SUBCASE("a non-empty output folder is refused")
    {
        const fs::path out = root / "busy";
        fs::create_directories(out);
        std::ofstream(out / "keep.txt") << "x";
        regions::ExportRequest request;
        request.outDir = out;
        request.level = 0;
        const auto result = regions::exportCrop(*volume, "vol", "local", region, map, request, {});
        CHECK_FALSE(result.ok());
        CHECK(result.error.find("not empty") != std::string::npos);
        CHECK(fs::exists(out / "keep.txt"));
    }
    SUBCASE("a cancel leaves neither the crop nor its partial folder")
    {
        std::atomic<bool> cancel{true};
        regions::Progress progress;
        progress.cancel = &cancel;
        regions::ExportRequest request;
        request.outDir = root / "canceled";
        request.level = 0;
        const auto result = regions::exportCrop(*volume, "vol", "local", region, map, request, progress);
        CHECK(result.canceled);
        CHECK_FALSE(fs::exists(request.outDir));
        for (const auto& entry : fs::directory_iterator(root)) {
            CHECK(entry.path().filename().string().find(".partial-") == std::string::npos);
        }
    }
    SUBCASE("the crop reopens with the source voxels and the manifest places it")
    {
        regions::ExportRequest request;
        request.outDir = root / "crop";
        request.level = 1;
        request.author = "tester";
        request.created = "2026-10-18T14:10:00Z";
        const auto result = regions::exportCrop(*volume, "vol", "local", region, map, request, {});
        REQUIRE_MESSAGE(result.ok(), result.error);
        REQUIRE(fs::exists(request.outDir / "manifest.json"));
        const auto range = regions::boxIndexRange(*regions::boxOf(region), map, 1, volume->shape(1));
        const std::array<std::size_t, 3> shape = {
            static_cast<std::size_t>(range.maxZyx[0] - range.minZyx[0] + 1),
            static_cast<std::size_t>(range.maxZyx[1] - range.minZyx[1] + 1),
            static_cast<std::size_t>(range.maxZyx[2] - range.minZyx[2] + 1),
        };
        Array3D<std::uint16_t> expected(shape);
        REQUIRE(volume->readZYX(expected, {range.minZyx[0], range.minZyx[1], range.minZyx[2]}, 1));
        auto crop = Volume::New(request.outDir / "volume.zarr");
        CHECK(crop->shape(0) == std::array<int, 3>{static_cast<int>(shape[0]), static_cast<int>(shape[1]), static_cast<int>(shape[2])});
        Array3D<std::uint16_t> actual(shape);
        REQUIRE(crop->readZYX(actual, {0, 0, 0}, 0));
        CHECK(actual.data_ == expected.data_);

        // Crop index (0, 0, 0) is annotation point i0 * 2^L / k.
        const auto& matrix = result.manifest["crop"]["crop_index_zyx_to_annotation_xyz"];
        const double s = 2.0 / 2.0;  // 2^L / k with L = 1, k = 2
        CHECK(result.manifest["crop"]["annotation_voxels_per_crop_voxel"].get<double>() == s);
        CHECK(matrix[0][3].get<double>() == range.minZyx[2] * s);
        CHECK(matrix[1][3].get<double>() == range.minZyx[1] * s);
        CHECK(matrix[2][3].get<double>() == range.minZyx[0] * s);
        CHECK(matrix[0][2].get<double>() == s);
        CHECK(result.manifest["schema"] == "vc3d.region_export");
        CHECK(result.manifest["region"]["id"] == region.id);
        // The default folder name has no colons.
        const fs::path suggested = regions::defaultExportDir(root, region.id, "2026-10-18T14:10:00Z");
        CHECK(suggested.filename().string().find(':') == std::string::npos);
        CHECK(suggested.filename().string().rfind("6f1c2c1e-20261018T141000Z-", 0) == 0);
    }
    fs::remove_all(root);
}
