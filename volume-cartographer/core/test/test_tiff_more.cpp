// More Tiff coverage: TiffWriter move-assignment, partial-tile padding,
// 32F dtype, atomicImwriteMulti error paths.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "vc/core/util/Tiff.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path tmpDir(const std::string& tag)
{
    std::mt19937_64 rng(std::random_device{}());
    auto p = fs::temp_directory_path() /
             ("vc_tiff_more_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("TiffWriter move-assignment transfers state")
{
    auto d = tmpDir("move_asn");
    auto p1 = d / "a.tif";
    auto p2 = d / "b.tif";
    TiffWriter w1(p1, 32, 32, CV_8UC1, 16, 16);
    cv::Mat tile(16, 16, CV_8UC1, cv::Scalar(7));
    w1.writeTile(0, 0, tile);

    TiffWriter w2(p2, 16, 16, CV_8UC1, 16, 16);
    w2 = std::move(w1); // move-assign closes the previous, takes ownership
    CHECK(w2.isOpen());
    CHECK_FALSE(w1.isOpen());
    w2.writeTile(16, 16, tile);
    w2.close();
    CHECK(fs::exists(p1));
    fs::remove_all(d);
}

TEST_CASE("TiffWriter self-move-assignment is a no-op")
{
    auto d = tmpDir("self_move");
    auto p = d / "x.tif";
    TiffWriter w(p, 16, 16, CV_8UC1, 16, 16);
    auto* same = &w;
    w = std::move(*same);  // self-assign
    CHECK(w.isOpen());
    w.close();
    fs::remove_all(d);
}

TEST_CASE("TiffWriter partial-tile write pads with default value")
{
    auto d = tmpDir("partial");
    auto p = d / "x.tif";
    {
        TiffWriter w(p, 24, 24, CV_8UC1, /*tileW=*/16, /*tileH=*/16);
        cv::Mat full(16, 16, CV_8UC1, cv::Scalar(50));
        w.writeTile(0, 0, full);
        // Bottom-right partial tile: 8x8 actual data
        cv::Mat partial(8, 8, CV_8UC1, cv::Scalar(100));
        w.writeTile(16, 16, partial);
    }
    REQUIRE(fs::exists(p));
    fs::remove_all(d);
}

TEST_CASE("TiffWriter rejects mismatched dtype on writeTile")
{
    auto d = tmpDir("dtype_mismatch");
    auto p = d / "x.tif";
    TiffWriter w(p, 16, 16, CV_8UC1, 16, 16);
    cv::Mat wrong(16, 16, CV_16UC1, cv::Scalar(0));
    CHECK_THROWS_AS(w.writeTile(0, 0, wrong), std::runtime_error);
    w.close();
    fs::remove_all(d);
}

TEST_CASE("TiffWriter rejects writeTile after close")
{
    auto d = tmpDir("after_close");
    auto p = d / "x.tif";
    TiffWriter w(p, 16, 16, CV_8UC1, 16, 16);
    w.close();
    cv::Mat tile(16, 16, CV_8UC1, cv::Scalar(0));
    CHECK_THROWS_AS(w.writeTile(0, 0, tile), std::runtime_error);
    fs::remove_all(d);
}

TEST_CASE("writeTiff with 32F dtype roundtrips")
{
    auto d = tmpDir("f32");
    auto p = d / "x.tif";
    cv::Mat_<float> img(16, 16, 3.14f);
    writeTiff(p, img, CV_32FC1, 16, 16);
    REQUIRE(fs::exists(p));
    auto loaded = cv::imread(p.string(), cv::IMREAD_UNCHANGED);
    REQUIRE(!loaded.empty());
    CHECK(loaded.type() == CV_32FC1);
    fs::remove_all(d);
}

TEST_CASE("writeTiff: 16U -> 8U and 32F -> 8U conversions")
{
    auto d = tmpDir("u16_to_u8");
    cv::Mat m16(32, 32, CV_16UC1, cv::Scalar(65535));
    writeTiff(d / "a.tif", m16, CV_8UC1, /*tileW=*/0, /*tileH=*/0);
    CHECK(fs::exists(d / "a.tif"));
    cv::Mat mf(32, 32, CV_32FC1, cv::Scalar(1.0f));
    writeTiff(d / "b.tif", mf, CV_8UC1, 0, 0);
    CHECK(fs::exists(d / "b.tif"));
    fs::remove_all(d);
}

TEST_CASE("writeTiff: 16U -> 32F and 32F -> 16U conversions")
{
    auto d = tmpDir("dtype_xform");
    cv::Mat m16(32, 32, CV_16UC1, cv::Scalar(32000));
    writeTiff(d / "a.tif", m16, CV_32FC1, 0, 0);
    CHECK(fs::exists(d / "a.tif"));
    cv::Mat mf(32, 32, CV_32FC1, cv::Scalar(0.5f));
    writeTiff(d / "b.tif", mf, CV_16UC1, 0, 0);
    CHECK(fs::exists(d / "b.tif"));
    fs::remove_all(d);
}

TEST_CASE("atomicImwriteMulti: throws on empty page vector")
{
    auto d = tmpDir("atomic_empty");
    auto p = d / "x.tif";
    std::vector<cv::Mat> empty;
    CHECK_THROWS_AS(atomicImwriteMulti(p, empty), std::runtime_error);
    fs::remove_all(d);
}

TEST_CASE("atomicImwriteMulti: writes multi-page TIFF atomically")
{
    auto d = tmpDir("atomic_ok");
    auto p = d / "x.tif";
    std::vector<cv::Mat> pages = {
        cv::Mat(8, 8, CV_8UC1, cv::Scalar(10)),
        cv::Mat(8, 8, CV_8UC1, cv::Scalar(20)),
    };
    atomicImwriteMulti(p, pages);
    REQUIRE(fs::exists(p));
    // No stray .tmp left over.
    CHECK_FALSE(fs::exists((d / "x.tmp.tif")));
    fs::remove_all(d);
}

TEST_CASE("voxelSizeToDpi: very tiny voxels produce large dpi")
{
    CHECK(voxelSizeToDpi(0.01) == doctest::Approx(2540000.0f).epsilon(1e-3));
}

// ---------------------------------------------------------------------------
// forEachTiffBlock: streaming block reader
// ---------------------------------------------------------------------------

namespace {

cv::Mat patternImage(int width, int height, int type)
{
    cv::Mat img(height, width, type);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int v = (x * 7 + y * 13) & 0xFFFF;
            if (CV_MAT_DEPTH(type) == CV_8U)
                img.at<uint8_t>(y, x) = uint8_t(v & 0xFF);
            else
                img.at<uint16_t>(y, x) = uint16_t(v);
        }
    return img;
}

// A striped (untiled) single-channel TIFF with `rowsPerStrip` rows per strip.
void writeStripedTiff(const fs::path& path, const cv::Mat& img, uint32_t rowsPerStrip)
{
    TIFF* tf = TIFFOpen(path.string().c_str(), "w");
    REQUIRE(tf != nullptr);
    const int bits = CV_MAT_DEPTH(img.type()) == CV_8U ? 8 : 16;
    TIFFSetField(tf, TIFFTAG_IMAGEWIDTH, uint32_t(img.cols));
    TIFFSetField(tf, TIFFTAG_IMAGELENGTH, uint32_t(img.rows));
    TIFFSetField(tf, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tf, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(tf, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tf, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tf, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tf, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(tf, TIFFTAG_ROWSPERSTRIP, rowsPerStrip);
    for (int y = 0; y < img.rows; ++y)
        REQUIRE(TIFFWriteScanline(tf, const_cast<uchar*>(img.ptr(y)), uint32_t(y), 0) == 1);
    TIFFClose(tf);
}

// Reassembles a file from its blocks and counts how often each pixel arrives.
struct Reassembled {
    TiffImageInfo info;
    cv::Mat image;
    cv::Mat_<uint8_t> visits;
    int blocks = 0;
    bool ok = false;
    std::string error;
};

Reassembled reassemble(const fs::path& path)
{
    Reassembled out;
    out.ok = forEachTiffBlock(
        path,
        [&out](const TiffImageInfo& info, cv::Point topLeft, const cv::Mat& block) {
            if (out.image.empty()) {
                out.info = info;
                out.image = cv::Mat(int(info.height), int(info.width), block.type(), cv::Scalar::all(0));
                out.visits = cv::Mat_<uint8_t>(int(info.height), int(info.width), uint8_t(0));
            }
            const cv::Rect rect(topLeft, block.size());
            REQUIRE((rect & cv::Rect(0, 0, out.image.cols, out.image.rows)) == rect);
            block.copyTo(out.image(rect));
            out.visits(rect) += 1;
            ++out.blocks;
            return true;
        },
        &out.error);
    return out;
}

}  // namespace

TEST_CASE("forEachTiffBlock visits every pixel of a tiled file exactly once")
{
    auto d = tmpDir("blocks_tiled");
    const cv::Mat img = patternImage(4097, 2049, CV_8UC1);
    writeTiff(d / "tiled.tif", img, -1, 1024, 1024, 0.f, COMPRESSION_LZW);
    const auto r = reassemble(d / "tiled.tif");
    REQUIRE(r.ok);
    CHECK(r.info.tiled);
    CHECK(r.info.width == 4097);
    CHECK(r.info.height == 2049);
    CHECK(r.info.samplesPerPixel == 1);
    CHECK(r.info.bitsPerSample == 8);
    CHECK(r.blocks == 5 * 3);
    CHECK(cv::countNonZero(r.visits != 1) == 0);
    CHECK(cv::countNonZero(r.image != img) == 0);
    fs::remove_all(d);
}

TEST_CASE("forEachTiffBlock streams striped 8 and 16 bit files, clipping the last strip")
{
    auto d = tmpDir("blocks_striped");
    for (const int type : {CV_8UC1, CV_16UC1}) {
        const cv::Mat img = patternImage(4097, 2049, type);
        writeStripedTiff(d / "striped.tif", img, 100);
        const auto r = reassemble(d / "striped.tif");
        REQUIRE(r.ok);
        CHECK_FALSE(r.info.tiled);
        CHECK(r.blocks == 21);  // 20 full strips and one of 49 rows
        CHECK(r.image.type() == type);
        CHECK(cv::countNonZero(r.visits != 1) == 0);
        cv::Mat diff;
        cv::absdiff(r.image, img, diff);
        CHECK(cv::countNonZero(diff.reshape(1)) == 0);
    }
    fs::remove_all(d);
}

TEST_CASE("forEachTiffBlock reports multi-sample layouts and refuses float data")
{
    auto d = tmpDir("blocks_layout");
    cv::Mat rgb(37, 53, CV_8UC3, cv::Scalar(10, 20, 30));
    REQUIRE(cv::imwrite((d / "rgb.tif").string(), rgb));
    const auto r = reassemble(d / "rgb.tif");
    REQUIRE(r.ok);
    CHECK(r.info.samplesPerPixel == 3);
    CHECK(r.image.type() == CV_8UC3);
    CHECK(cv::countNonZero(r.visits != 1) == 0);

    cv::Mat f(16, 16, CV_32FC1, cv::Scalar(0.5f));
    writeTiff(d / "float.tif", f, -1, 16, 16);
    std::string error;
    int calls = 0;
    CHECK_FALSE(forEachTiffBlock(d / "float.tif", [&calls](const TiffImageInfo&, cv::Point, const cv::Mat&) {
        ++calls;
        return true;
    }, &error));
    CHECK(calls == 0);
    CHECK(error.find("sample format") != std::string::npos);
    fs::remove_all(d);
}

TEST_CASE("forEachTiffBlock fails on a truncated file and stops when asked")
{
    auto d = tmpDir("blocks_truncated");
    const cv::Mat img = patternImage(2048, 2048, CV_8UC1);
    writeTiff(d / "full.tif", img, -1, 1024, 1024, 0.f, COMPRESSION_LZW);
    const auto size = fs::file_size(d / "full.tif");
    fs::copy_file(d / "full.tif", d / "torn.tif");
    fs::resize_file(d / "torn.tif", size / 2);
    std::string error;
    CHECK_FALSE(forEachTiffBlock(d / "torn.tif", [](const TiffImageInfo&, cv::Point, const cv::Mat&) { return true; }, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(isReadableTiff(d / "torn.tif"));
    CHECK(isReadableTiff(d / "full.tif"));

    int calls = 0;
    error.clear();
    CHECK_FALSE(forEachTiffBlock(d / "full.tif", [&calls](const TiffImageInfo&, cv::Point, const cv::Mat&) {
        return ++calls < 2;
    }, &error));
    CHECK(calls == 2);
    CHECK(error.find("stopped") != std::string::npos);
    CHECK_FALSE(forEachTiffBlock(d / "absent.tif", [](const TiffImageInfo&, cv::Point, const cv::Mat&) { return true; }));
    fs::remove_all(d);
}
