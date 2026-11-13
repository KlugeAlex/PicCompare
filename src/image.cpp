#include "image.h"

#include <wrl/client.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace pic {
using Microsoft::WRL::ComPtr;

Point Orientation::size(int w, int h) const {
    return {double(std::abs(xx) * w + std::abs(xy) * h), double(std::abs(yx) * w + std::abs(yy) * h)};
}

Point Orientation::offset(int w, int h) const {
    return {double((xx < 0 ? w : 0) + (xy < 0 ? h : 0)), double((yx < 0 ? w : 0) + (yy < 0 ? h : 0))};
}

Point Orientation::forward(Point p, int w, int h) const {
    return Point{xx * p.x + xy * p.y, yx * p.x + yy * p.y} + offset(w, h);
}

Point Orientation::inverse(Point p, int w, int h) const {
    p = p - offset(w, h);
    return {xx * p.x + yx * p.y, xy * p.x + yy * p.y};
}

void Orientation::apply(wchar_t key) {
    const auto previous = *this;
    switch (key) {
    case L'O': *this = {-previous.yx, -previous.yy, previous.xx, previous.xy}; break;
    case L'I': xx = -xx; xy = -xy; break;
    case L'U': yx = -yx; yy = -yy; break;
    }
}

const Pixel* Image::at(int x, int y) const {
    if (x < 0 || x >= w || y < 0 || y >= h || pixels.empty()) return nullptr;
    return &pixels[size_t(y) * w + x];
}

const Pixel* Image::oriented(int x, int y, Orientation orientation) const {
    const Point p = orientation.inverse({x + .5, y + .5}, w, h);
    return at(int(std::floor(p.x)), int(std::floor(p.y)));
}

int nativeValue(uint16_t sample, int bits) {
    return int((uint64_t(sample) * ((1u << bits) - 1) + 32767) / 65535);
}

static std::wstring extension(const fs::path& p) {
    auto result = p.extension().wstring();
    for (auto& ch : result) ch = wchar_t(towlower(ch));
    return result;
}

bool supported(const fs::path& path) {
    const auto ext = extension(path);
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".png" ||
        ext == L".bmp" || ext == L".tif" || ext == L".tiff" || ext == L".webp" ||
        ext == L".gif" || ext == L".ico" || ext == L".jfif" || ext == L".dib";
}

static void check(HRESULT hr, const wchar_t* message) {
    if (FAILED(hr)) throw std::wstring(message);
}

static void checkSize(uint32_t w, uint32_t h) {
    if (!w || !h || uint64_t(w) * h > 100000000)
        throw std::wstring(L"图像为空或超过 1 亿像素。");
}

static uint32_t displayPixel(uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
    const uint32_t alpha = (uint32_t(a) + 128) / 257;
    const auto premul = [alpha](uint16_t channel) { return (uint32_t(channel) * alpha + 32767) / 65535; };
    return alpha << 24 | premul(r) << 16 | premul(g) << 8 | premul(b);
}

static void buildDisplay(Image& image) {
    auto raster = std::make_shared<Raster>();
    raster->w = image.w;
    raster->h = image.h;
    raster->bgra.resize(image.pixels.size());
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const auto& p = image.pixels[i];
        raster->bgra[i] = displayPixel(p.r, p.g, p.b, p.a);
        if (!p.a) continue;
        ++image.histogram[0][p.r >> 8];
        ++image.histogram[1][p.g >> 8];
        ++image.histogram[2][p.b >> 8];
    }
    image.display = std::move(raster);
}

static ComPtr<IWICBitmapFrameDecode> openFrame(IWICImagingFactory* wic, const fs::path& path) {
    if (!wic) throw std::wstring(L"无法初始化 Windows 图像解码器。");
    ComPtr<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
        &decoder), L"无法解码图像。请检查文件是否损坏或格式是否受支持。");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), L"无法读取第一帧。");
    return frame;
}

static ComPtr<IWICFormatConverter> convert(IWICImagingFactory* wic, IWICBitmapSource* source,
    const WICPixelFormatGUID& format) {
    ComPtr<IWICFormatConverter> converter;
    check(wic->CreateFormatConverter(&converter), L"无法初始化像素转换器。");
    check(converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0,
        WICBitmapPaletteTypeCustom), L"不支持此像素格式。");
    return converter;
}

static int exifOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> meta;
    if (FAILED(frame->GetMetadataQueryReader(&meta))) return 1;
    for (auto name : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT value{};
        if (SUCCEEDED(meta->GetMetadataByName(name, &value))) {
            const int tag = value.vt == VT_UI2 ? value.uiVal : 1;
            PropVariantClear(&value);
            return tag;
        }
    }
    return 1;
}

static Orientation fromExif(int tag) {
    Orientation o;
    switch (tag) {
    case 2: o.apply(L'I'); break;
    case 3: o.apply(L'O'); o.apply(L'O'); break;
    case 4: o.apply(L'U'); break;
    case 5: o.apply(L'O'); o.apply(L'I'); break;
    case 6: o.apply(L'O'); break;
    case 7: o.apply(L'O'); o.apply(L'U'); break;
    case 8: o.apply(L'O'); o.apply(L'O'); o.apply(L'O'); break;
    }
    return o;
}

static void orientExif(Image& image, int tag) {
    if (tag <= 1 || tag > 8) return;
    const Orientation orientation = fromExif(tag);
    const auto size = orientation.size(image.w, image.h);
    std::vector<Pixel> ordered(image.pixels.size());
    for (int y = 0; y < int(size.y); ++y)
        for (int x = 0; x < int(size.x); ++x)
            ordered[size_t(y) * int(size.x) + x] = *image.oriented(x, y, orientation);
    image.w = int(size.x);
    image.h = int(size.y);
    image.pixels = std::move(ordered);
}

static void decodeWic(IWICImagingFactory* wic, const fs::path& path, Image& image) {
    auto frame = openFrame(wic, path);
    UINT w{}, h{};
    check(frame->GetSize(&w, &h), L"无法读取图像尺寸。");
    checkSize(w, h);
    WICPixelFormatGUID format{};
    check(frame->GetPixelFormat(&format), L"无法读取像素格式。");
    image.w = int(w);
    image.h = int(h);
    const bool gray16 = format == GUID_WICPixelFormat16bppGray;
    image.gray = gray16 || format == GUID_WICPixelFormat8bppGray ||
        format == GUID_WICPixelFormat4bppGray || format == GUID_WICPixelFormat2bppGray ||
        format == GUID_WICPixelFormatBlackWhite;
    image.bits = format == GUID_WICPixelFormatBlackWhite ? 1 :
        format == GUID_WICPixelFormat2bppGray ? 2 : format == GUID_WICPixelFormat4bppGray ? 4 : 8;

    ComPtr<IWICComponentInfo> component;
    ComPtr<IWICPixelFormatInfo2> info;
    if (SUCCEEDED(wic->CreateComponentInfo(format, &component)) && SUCCEEDED(component.As(&info))) {
        WICPixelFormatNumericRepresentation numeric{};
        if (SUCCEEDED(info->GetNumericRepresentation(&numeric)) &&
            (numeric == WICPixelFormatNumericRepresentationFloat ||
                numeric == WICPixelFormatNumericRepresentationFixed))
            throw std::wstring(L"当前支持整数样本，无法测量浮点或定点图像。");
        if (numeric == WICPixelFormatNumericRepresentationUnsignedInteger) {
            UINT channels{};
            info->GetChannelCount(&channels);
            for (UINT i = 0; i < (std::min)(channels, 3u); ++i) {
                BYTE mask[32]{};
                UINT used{};
                if (FAILED(info->GetChannelMask(i, sizeof(mask), mask, &used))) continue;
                int bits{};
                for (UINT k = 0; k < used && k < sizeof(mask); ++k)
                    for (unsigned bit = mask[k]; bit; bit >>= 1) bits += int(bit & 1);
                if (bits > 8) image.bits = (std::max)(image.bits, (std::min)(bits, 16));
            }
        }
    }
    const size_t count = size_t(w) * h;
    image.pixels.resize(count);
    if (gray16) {
        image.bits = 16;
        std::vector<uint16_t> samples(count);
        check(frame->CopyPixels(nullptr, w * 2, UINT(count * 2), reinterpret_cast<BYTE*>(samples.data())),
            L"无法读取 16 位灰度像素。");
        for (size_t i = 0; i < count; ++i) image.pixels[i] = {samples[i], samples[i], samples[i], 65535};
    } else if (image.bits > 8) {
        auto pixels = convert(wic, frame.Get(), GUID_WICPixelFormat64bppRGBA);
        check(pixels->CopyPixels(nullptr, w * sizeof(Pixel), UINT(count * sizeof(Pixel)),
            reinterpret_cast<BYTE*>(image.pixels.data())), L"无法读取高位深 RGB 像素。");
    } else {
        auto pixels = convert(wic, frame.Get(), GUID_WICPixelFormat32bppRGBA);
        std::vector<uint8_t> bytes(count * 4);
        check(pixels->CopyPixels(nullptr, w * 4, UINT(bytes.size()), bytes.data()), L"无法读取像素。");
        for (size_t i = 0; i < count; ++i) {
            const auto* p = &bytes[i * 4];
            image.pixels[i] = {uint16_t(p[0] * 257), uint16_t(p[1] * 257),
                uint16_t(p[2] * 257), uint16_t(p[3] * 257)};
        }
    }
    orientExif(image, exifOrientation(frame.Get()));
}

static std::vector<uint8_t> readWebP(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::wstring(L"无法打开 WebP 文件。");
    const auto length = input.tellg();
    if (length < 1 || length > 512 * 1024 * 1024)
        throw std::wstring(L"WebP 文件为空或超过 512 MB。");
    std::vector<uint8_t> data(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), length)) throw std::wstring(L"无法读取 WebP 文件。");
    return data;
}

static void decodeWebP(const fs::path& path, Image& image) {
    const auto bytes = readWebP(path);
    const WebPData data{bytes.data(), bytes.size()};
    WebPAnimDecoderOptions options{};
    WebPAnimDecoderOptionsInit(&options);
    options.color_mode = MODE_RGBA;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)>
        decoder(WebPAnimDecoderNew(&data, &options), &WebPAnimDecoderDelete);
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info))
        throw std::wstring(L"无法解码 WebP 图像。");
    checkSize(info.canvas_width, info.canvas_height);
    uint8_t* rgba{};
    int timestamp{};
    if (!WebPAnimDecoderGetNext(decoder.get(), &rgba, &timestamp))
        throw std::wstring(L"无法读取 WebP 第一帧。");
    image.w = int(info.canvas_width);
    image.h = int(info.canvas_height);
    image.pixels.resize(size_t(image.w) * image.h);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const auto* p = &rgba[4 * i];
        image.pixels[i] = {uint16_t(p[0] * 257), uint16_t(p[1] * 257),
            uint16_t(p[2] * 257), uint16_t(p[3] * 257)};
    }
}

std::shared_ptr<Image> decode(IWICImagingFactory* wic, const fs::path& path) {
    auto image = std::make_shared<Image>();
    try {
        if (extension(path) == L".webp") decodeWebP(path, *image);
        else decodeWic(wic, path, *image);
        buildDisplay(*image);
    } catch (const std::wstring& error) {
        image = std::make_shared<Image>();
        image->error = error;
    } catch (const std::bad_alloc&) {
        image = std::make_shared<Image>();
        image->error = L"内存不足，无法载入此图像。";
    }
    return image;
}

std::shared_ptr<Raster> thumbnail(IWICImagingFactory* wic, const fs::path& path) {
    constexpr double edge = 224;
    try {
        auto raster = std::make_shared<Raster>();
        if (extension(path) == L".webp") {
            const auto bytes = readWebP(path);
            WebPDecoderConfig config{};
            if (!WebPInitDecoderConfig(&config) ||
                WebPGetFeatures(bytes.data(), bytes.size(), &config.input) != VP8_STATUS_OK) return {};
            const double fit = (std::min)(1., edge / (std::max)(config.input.width, config.input.height));
            raster->w = (std::max)(1, int(config.input.width * fit));
            raster->h = (std::max)(1, int(config.input.height * fit));
            if (config.input.has_animation) {
                Image first;
                decodeWebP(path, first);
                raster->bgra.resize(size_t(raster->w) * raster->h);
                for (int y = 0; y < raster->h; ++y) for (int x = 0; x < raster->w; ++x) {
                    const auto* p = first.at(x * first.w / raster->w, y * first.h / raster->h);
                    raster->bgra[size_t(y) * raster->w + x] = displayPixel(p->r, p->g, p->b, p->a);
                }
                return raster;
            }
            raster->bgra.resize(size_t(raster->w) * raster->h);
            config.options.use_scaling = 1;
            config.options.scaled_width = raster->w;
            config.options.scaled_height = raster->h;
            config.output.colorspace = MODE_bgrA;
            config.output.is_external_memory = 1;
            config.output.u.RGBA = {reinterpret_cast<uint8_t*>(raster->bgra.data()),
                raster->w * 4, raster->bgra.size() * 4};
            const auto result = WebPDecode(bytes.data(), bytes.size(), &config);
            WebPFreeDecBuffer(&config.output);
            return result == VP8_STATUS_OK ? raster : nullptr;
        }

        auto frame = openFrame(wic, path);
        UINT w{}, h{};
        check(frame->GetSize(&w, &h), L"无法读取尺寸。");
        if (!w || !h) return {};
        const double fit = (std::min)(1., edge / (std::max)(w, h));
        const UINT sw = (std::max)(1u, UINT(w * fit)), sh = (std::max)(1u, UINT(h * fit));
        ComPtr<IWICBitmapScaler> scaler;
        check(wic->CreateBitmapScaler(&scaler), L"无法创建缩略图。");
        check(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant), L"无法缩放缩略图。");
        auto converted = convert(wic, scaler.Get(), GUID_WICPixelFormat32bppPBGRA);
        std::vector<uint32_t> pixels(size_t(sw) * sh);
        check(converted->CopyPixels(nullptr, sw * 4, UINT(pixels.size() * 4),
            reinterpret_cast<BYTE*>(pixels.data())), L"无法读取缩略图。");
        const auto o = fromExif(exifOrientation(frame.Get()));
        const auto size = o.size(sw, sh);
        raster->w = int(size.x);
        raster->h = int(size.y);
        raster->bgra.resize(pixels.size());
        for (int y = 0; y < raster->h; ++y) for (int x = 0; x < raster->w; ++x) {
            const auto p = o.inverse({x + .5, y + .5}, sw, sh);
            raster->bgra[size_t(y) * raster->w + x] = pixels[size_t(p.y) * sw + size_t(p.x)];
        }
        return raster;
    } catch (...) {
        return {};
    }
}

static double luma(const Pixel& p) {
    return (.2126 * p.r + .7152 * p.g + .0722 * p.b) / 65535.;
}

static const std::array<double, 65536>& linearSRGB() {
    static const auto table = [] {
        std::array<double, 65536> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const double v = i / 65535.;
            values[i] = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
        }
        return values;
    }();
    return table;
}

static double labCurve(double v) {
    return v > .008856451679 ? std::cbrt(v) : 7.787037037 * v + 16. / 116.;
}

Stats measure(const Image& image, Orientation orientation, Box normalized) {
    const auto size = orientation.size(image.w, image.h);
    const auto index = [](double position, double dimension) {
        return std::clamp(int(std::ceil(position * dimension - .5 - 1e-9)), 0, int(dimension));
    };
    const int x0 = index(normalized.l, size.x), x1 = index(normalized.r, size.x);
    const int y0 = index(normalized.t, size.y), y1 = index(normalized.b, size.y);
    Stats result{x0, y0, x1 - x0, y1 - y0};
    double mean{}, m2{}, hs{}, hc{}, hueWeight{};
    const auto& linear = linearSRGB();
    const double maxValue = (1u << image.bits) - 1;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const Pixel* p = image.oriented(x, y, orientation);
        if (!p || !p->a) continue;
        ++result.count;
        const double r = p->r / 65535., g = p->g / 65535., b = p->b / 65535.;
        result.rgb[0] += r * maxValue;
        result.rgb[1] += g * maxValue;
        result.rgb[2] += b * maxValue;
        const double lr = linear[p->r], lg = linear[p->g], lb = linear[p->b];
        const double fx = labCurve((.4124564 * lr + .3575761 * lg + .1804375 * lb) / .95047);
        const double fy = labCurve(.2126729 * lr + .7151522 * lg + .0721750 * lb);
        const double fz = labCurve((.0193339 * lr + .1191920 * lg + .9503041 * lb) / 1.08883);
        result.lab[0] += 116 * fy - 16;
        result.lab[1] += 500 * (fx - fy);
        result.lab[2] += 200 * (fy - fz);

        const double hi = (std::max)({r, g, b}), lo = (std::min)({r, g, b});
        const double delta = hi - lo;
        const double saturation = hi > 0 ? delta / hi : 0;
        if (delta > 0) {
            const double sector = hi == r ? (g - b) / delta :
                hi == g ? (b - r) / delta + 2 : (r - g) / delta + 4;
            const double angle = sector * std::numbers::pi / 3;
            hs += std::sin(angle) * saturation;
            hc += std::cos(angle) * saturation;
            hueWeight += saturation;
        }
        result.hsv[1] += saturation * 100;
        result.hsv[2] += hi * 100;
        const double value = luma(*p) * maxValue;
        const double step = value - mean;
        mean += step / double(result.count);
        m2 += step * (value - mean);
    }
    if (result.count) {
        for (auto* array : {&result.rgb, &result.lab, &result.hsv})
            for (auto& value : *array) value /= double(result.count);
        result.hueValid = hueWeight > 0 && std::hypot(hs, hc) > hueWeight * 1e-8;
        result.hsv[0] = std::fmod(std::atan2(hs, hc) * 180 / std::numbers::pi + 360, 360);
        if (result.count > 1) result.sigma = std::sqrt((std::max)(0., m2) / double(result.count - 1));
    }
    return result;
}

std::vector<float> difference(const Image& left, Orientation a, const Image& right, Orientation b) {
    std::vector<float> values(size_t(right.w) * right.h, std::numeric_limits<float>::quiet_NaN());
    const auto ls = a.size(left.w, left.h), rs = b.size(right.w, right.h);
    for (int y = 0; y < int((std::min)(ls.y, rs.y)); ++y) {
        for (int x = 0; x < int((std::min)(ls.x, rs.x)); ++x) {
            const auto p = b.inverse({x + .5, y + .5}, right.w, right.h);
            const auto& r = right.pixels[size_t(p.y) * right.w + size_t(p.x)];
            const auto* l = left.oriented(x, y, a);
            if (l && l->a && r.a) values[size_t(p.y) * right.w + size_t(p.x)] = float(255 * (luma(r) - luma(*l)));
        }
    }
    return values;
}

std::shared_ptr<Raster> differenceRaster(const Image& right,
    const std::vector<float>& values, double gain) {
    auto raster = std::make_shared<Raster>();
    raster->w = right.w;
    raster->h = right.h;
    raster->bgra.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) continue;
        const uint32_t v = uint32_t(std::lround(std::clamp(128. + gain * values[i], 0., 255.)));
        raster->bgra[i] = 0xff000000u | v * 0x010101u;
    }
    return raster;
}

} // namespace pic
