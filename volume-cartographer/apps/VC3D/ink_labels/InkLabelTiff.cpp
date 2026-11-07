#include "ink_labels/InkLabelTiff.hpp"

#include "vc/core/util/ImageMat.hpp"

#include <vc_reader/image_write.hpp>
#include <vc_reader/source_reader.hpp>
#include <vc_reader/sources.hpp>
#include <vc_reader/zarr.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace vc3d::ink_labels {

namespace {

namespace src = vcr::sources;

struct OpenedImage {
    std::unique_ptr<src::SourceReader> reader;
    ImageBandInfo info;
    std::string dtype;
    std::vector<std::string> spatial;
};

bool fail(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

bool knownImageSuffix(const std::filesystem::path& path)
{
    std::string suffix = path.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return suffix == ".tif" || suffix == ".tiff" || suffix == ".png" || suffix == ".jpg" || suffix == ".jpeg";
}

bool openImage(const std::filesystem::path& path, OpenedImage& out, std::string* error,
               const std::optional<std::vector<std::string>>& zarrAxes = std::nullopt)
{
    const std::string location = path.string();
    try {
        src::SourceReader::Settings settings;
        settings.decodedCacheBytes = 0;
        settings.maxDecodeBytes = std::numeric_limits<std::size_t>::max() / 2;
        src::Source source;
        if (zarrAxes) {
            source.id = location;
            source.storage.location = location;
            source.storage.format = "zarr";
            source.role = "scalar";
            source.axes = *zarrAxes;
            source = src::finished(std::move(source));
        } else if (knownImageSuffix(path)) {
            source = src::imageSource(location);
        } else {
            // Our own single-channel TIFFs under another name (a checkpoint's
            // ".tif.autosave"), which the suffix check would not recognize.
            source.id = location;
            source.storage.location = location;
            source.storage.format = "tiff";
            source.storage.selection.series = std::int64_t(0);
            source.storage.selection.level = std::int64_t(0);
            source.role = "scalar";
            source.axes = std::vector<std::string>{"y", "x"};
            source = src::finished(std::move(source));
        }
        out.reader = std::make_unique<src::SourceReader>(std::move(source), std::move(settings));
        const auto& axes = out.reader->axes();
        const auto& shape = out.reader->shape();
        out.spatial = out.reader->spatialAxes();
        out.dtype = out.reader->dtype();
        const auto extent = [&](const std::string& name) -> std::int64_t {
            const auto it = std::find(axes.begin(), axes.end(), name);
            return it == axes.end() ? -1 : shape[std::size_t(it - axes.begin())];
        };
        const std::int64_t rows = extent("y"), cols = extent("x");
        if (rows <= 0 || cols <= 0 || rows > std::numeric_limits<int>::max() || cols > std::numeric_limits<int>::max())
            return fail(error, "not a 2-D image: " + location);
        for (const auto& axis : out.spatial)
            if (axis != "y" && axis != "x" && axis != "z" && extent(axis) != 1)
                return fail(error, "image has more than one page or plane (" + axis + "): " + location);
        if (!zarrAxes && extent("z") > 1)
            return fail(error, "image has more than one page: " + location);
        std::int64_t channels = 1;
        for (std::size_t a = 0; a < axes.size(); ++a)
            if (std::find(out.spatial.begin(), out.spatial.end(), axes[a]) == out.spatial.end())
                channels *= shape[a];
        if (channels < 1 || channels > CV_CN_MAX)
            return fail(error, "unsupported sample count " + std::to_string(channels) + ": " + location);
        out.info.width = int(cols);
        out.info.height = int(rows);
        out.info.channels = int(channels);
        if (out.dtype == "|u1" || out.dtype == "|b1")
            out.info.depth = CV_8U;
        else if (out.dtype == "<u2")
            out.info.depth = CV_16U;
        else
            return fail(error, "unsupported sample type " + out.dtype + " (only 8 or 16 bit unsigned): " + location);
        return true;
    } catch (const std::exception& e) {
        return fail(error, "cannot read " + location + ": " + e.what());
    }
}

}  // namespace

bool readImageBandInfo(const std::filesystem::path& path, ImageBandInfo& info, std::string* error)
{
    OpenedImage image;
    if (!openImage(path, image, error))
        return false;
    info = image.info;
    return true;
}

namespace {

bool walkBands(const OpenedImage& image, std::int64_t plane, const std::filesystem::path& path,
               const std::function<bool(const ImageBandInfo&, cv::Point, const cv::Mat&)>& onBand, std::string* error,
               int bandRows)
{
    const ImageBandInfo& info = image.info;
    bandRows = std::max(1, bandRows);
    try {
        for (int y = 0; y < info.height; y += bandRows) {
            const int h = std::min(bandRows, info.height - y);
            std::vector<std::int64_t> start, size;
            for (const auto& axis : image.spatial) {
                start.push_back(axis == "y" ? y : axis == "z" ? plane : 0);
                size.push_back(axis == "y" ? h : axis == "x" ? info.width : 1);
            }
            src::Block block = image.reader->read(start, size);
            cv::Mat band;
            if (image.dtype == "|b1") {
                block.shape = {h, info.width};
                band = vc::core::util::imageMat(block);
                if (band.empty() || band.channels() != info.channels)
                    return fail(error, "unexpected 1-bit layout: " + path.string());
            } else {
                const int type = CV_MAKETYPE(info.depth, info.channels);
                const std::size_t expected = std::size_t(h) * std::size_t(info.width) * std::size_t(info.channels)
                                             * (info.depth == CV_8U ? 1u : 2u);
                if (block.nbytes != expected)
                    return fail(error, "short read of " + path.string());
                band = cv::Mat(h, info.width, type, block.data.get());
            }
            if (!onBand(info, cv::Point(0, y), band))
                return fail(error, "stopped");
        }
        return true;
    } catch (const std::exception& e) {
        return fail(error, "cannot read " + path.string() + ": " + e.what());
    }
}

}  // namespace

bool forEachImageBand(const std::filesystem::path& path,
                      const std::function<bool(const ImageBandInfo&, cv::Point, const cv::Mat&)>& onBand,
                      std::string* error, int bandRows)
{
    OpenedImage image;
    if (!openImage(path, image, error))
        return false;
    return walkBands(image, 0, path, onBand, error, bandRows);
}

bool readZarrArrayShape(const std::filesystem::path& array, std::vector<std::int64_t>& shape, std::string& dtype,
                        std::string* error)
{
    try {
        auto opened = vcr::zarr::openArray(array.string(), vcr::zarr::OpenOptions{});
        shape.assign(opened.meta.shape.begin(), opened.meta.shape.end());
        dtype = opened.meta.dtype;
        opened.close();
        return true;
    } catch (const std::exception& e) {
        return fail(error, e.what());
    }
}

bool forEachZarrBand(const std::filesystem::path& array, std::int64_t plane,
                     const std::function<bool(const ImageBandInfo&, cv::Point, const cv::Mat&)>& onBand,
                     std::string* error, int bandRows)
{
    std::vector<std::int64_t> shape;
    std::string dtype;
    if (!readZarrArrayShape(array, shape, dtype, error))
        return false;
    if (shape.size() != 2 && shape.size() != 3)
        return fail(error, "expected a 2-D array or a (planes, H, W) label zarr");
    if (shape.size() == 3 && (plane < 0 || plane >= shape[0]))
        return fail(error, "plane " + std::to_string(plane) + " is outside the array");
    OpenedImage image;
    const std::vector<std::string> axes = shape.size() == 3 ? std::vector<std::string>{"z", "y", "x"}
                                                            : std::vector<std::string>{"y", "x"};
    if (!openImage(array, image, error, axes))
        return false;
    return walkBands(image, plane, array, onBand, error, bandRows);
}

bool writeTiledU8Tiff(const std::filesystem::path& path, cv::Size size, int tile,
                      const std::function<void(const cv::Rect&, cv::Mat_<uint8_t>&)>& fill, std::string* error)
{
    try {
        src::TiffOptions options;
        options.compression = "lzw";
        options.tile = std::array<int, 2>{tile, tile};
        src::TiffWriter writer(path, "|u1", size.height, size.width, 1, options);
        const auto [tileRows, tileCols] = writer.tile();
        cv::Mat_<uint8_t> buffer(tileRows, tileCols);
        for (int by = 0; by < size.height; by += tileRows)
            for (int bx = 0; bx < size.width; bx += tileCols) {
                const cv::Rect rect(bx, by, std::min(tileCols, size.width - bx), std::min(tileRows, size.height - by));
                cv::Mat_<uint8_t> block(rect.height, rect.width, buffer.ptr<uint8_t>(), std::size_t(rect.width));
                fill(rect, block);
                writer.writeTile(by, bx, block.ptr<uint8_t>(), rect.height, rect.width);
            }
        writer.finish();
        return true;
    } catch (const std::exception& e) {
        return fail(error, e.what());
    }
}

}  // namespace vc3d::ink_labels
