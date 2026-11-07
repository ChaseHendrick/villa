#pragma once

// Label image files through vc_reader: band-by-band reading of a TIFF, PNG or
// JPEG, and tiled uint8 TIFF writing. GUI-free.

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace vc3d::ink_labels {

struct ImageBandInfo {
    int width = 0, height = 0, channels = 1;
    int depth = CV_8U;  // CV_8U or CV_16U
};

// Reads the image's first page in bands of `bandRows` full-width rows and
// passes each band, in file sample order and valid only during the call, with
// its top-left pixel. 8 or 16 bit unsigned samples (1-bit samples arrive as
// 0 and 255). Memory use is one band, whatever the image height. Returns false
// (message in *error) if the file does not read, its samples are another type,
// or onBand returns false.
bool forEachImageBand(const std::filesystem::path& path,
                      const std::function<bool(const ImageBandInfo&, cv::Point topLeft, const cv::Mat& band)>& onBand,
                      std::string* error = nullptr, int bandRows = 1024);

// A local zarr array (not a group): its shape (C order) and numpy dtype.
bool readZarrArrayShape(const std::filesystem::path& array, std::vector<std::int64_t>& shape, std::string& dtype,
                        std::string* error = nullptr);

// The same band walk over a 2-D zarr array, or over plane `plane` of the first
// axis of a 3-D one (a (65, H, W) label zarr). uint8 or uint16 only.
bool forEachZarrBand(const std::filesystem::path& array, std::int64_t plane,
                     const std::function<bool(const ImageBandInfo&, cv::Point topLeft, const cv::Mat& band)>& onBand,
                     std::string* error = nullptr, int bandRows = 1024);

// The image's size and sample layout, without reading its pixels.
bool readImageBandInfo(const std::filesystem::path& path, ImageBandInfo& info, std::string* error = nullptr);

// A single-channel uint8 TIFF of `size`, tiled `tile` x `tile`, LZW, written
// through a temporary file and renamed into place by vc_reader's TiffWriter.
// `fill` writes the tile at `rect` into `block` (rect.size()).
bool writeTiledU8Tiff(const std::filesystem::path& path, cv::Size size, int tile,
                      const std::function<void(const cv::Rect& rect, cv::Mat_<uint8_t>& block)>& fill,
                      std::string* error = nullptr);

}  // namespace vc3d::ink_labels
