#pragma once

#include <windows.h>
#include <wincodec.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pic {
namespace fs = std::filesystem;

struct Point {
    double x{}, y{};
    Point operator+(Point p) const { return {x + p.x, y + p.y}; }
    Point operator-(Point p) const { return {x - p.x, y - p.y}; }
    Point operator*(double n) const { return {x * n, y * n}; }
};

struct Box {
    double l{}, t{}, r{}, b{};
    double width() const { return r - l; }
    double height() const { return b - t; }
    Point center() const { return {(l + r) / 2, (t + b) / 2}; }
    bool contains(Point p) const { return p.x >= l && p.x < r && p.y >= t && p.y < b; }
};

struct Orientation {
    int xx{1}, xy{}, yx{}, yy{1};
    Point size(int w, int h) const;
    Point offset(int w, int h) const;
    Point forward(Point p, int w, int h) const;
    Point inverse(Point p, int w, int h) const;
    void apply(wchar_t key);
};

struct Pixel {
    uint16_t r{}, g{}, b{}, a{65535};
};

struct Raster {
    int w{}, h{};
    std::vector<uint32_t> bgra;
};

struct Image {
    int w{}, h{}, bits{8};
    bool gray{};
    std::vector<Pixel> pixels;
    std::shared_ptr<Raster> display;
    std::array<std::array<uint64_t, 256>, 3> histogram{};
    std::wstring error;

    const Pixel* at(int x, int y) const;
    const Pixel* oriented(int x, int y, Orientation orientation) const;
};

struct Stats {
    int x{}, y{}, w{}, h{};
    uint64_t count{};
    std::array<double, 3> rgb{}, lab{}, hsv{};
    double sigma{};
    bool hueValid{};
};

bool supported(const fs::path& path);
std::shared_ptr<Image> decode(IWICImagingFactory* wic, const fs::path& path);
std::shared_ptr<Raster> thumbnail(IWICImagingFactory* wic, const fs::path& path);
Stats measure(const Image& image, Orientation orientation, Box normalized);
std::vector<float> difference(const Image& left, Orientation a, const Image& right, Orientation b);
std::shared_ptr<Raster> differenceRaster(const Image& right,
    const std::vector<float>& values, double gain);
int nativeValue(uint16_t sample, int bits);

} // namespace pic
