#include "../src/image.h"
#include "../src/interaction.h"
#include <wrl/client.h>
#include <cmath>
using namespace pic;
using Microsoft::WRL::ComPtr;
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}
bool close(double a, double b, double epsilon = 1e-7) {
    return std::abs(a - b) < epsilon;
}
void checkInteraction() {
    std::vector<int> order{1, 2, 3, 4};
    require(moveBefore(order, 0, 4) && order == std::vector<int>{2, 3, 4, 1}, "reorder forward to end");
    require(moveBefore(order, 2, 0) && order == std::vector<int>{4, 2, 3, 1}, "reorder backward to beginning");
    require(!moveBefore(order, 1, 2) && !moveBefore(order, 1, 5) && !moveBefore(order, 4, 0),
        "reorder no-op and invalid boundaries");
    const auto sameBox = [](Box a, Box b) {
        return close(a.l, b.l) && close(a.t, b.t) && close(a.r, b.r) && close(a.b, b.b);
    };
    std::array<Point, maxCompare> pixels;
    pixels.fill({.01, .01});
    const Box initial{.2, .2, .5, .5};
    Roi roi(initial);
    roi = editRoi(roi, 1, 2, true, {.1, .05}, pixels, 0);
    require(sameBox(roi.boxes[0], initial) && sameBox(roi.boxes[1], {.3, .25, .6, .55}),
        "independent ROI move");
    roi = editRoi(roi, 1, 2, true, {.1, .05}, pixels, RoiRight | RoiBottom);
    require(sameBox(roi.boxes[0], initial) && sameBox(roi.boxes[1], {.3, .25, .7, .6}),
        "independent ROI corner resize");
    roi = editRoi(roi, 0, 2, false, {.05, -.05}, pixels, 0);
    require(sameBox(roi.boxes[0], {.25, .15, .55, .45}) && sameBox(roi.boxes[1], {.35, .2, .75, .55}),
        "linked ROI move preserves local edits");
    roi = Roi({.1, .1, .4, .4});
    require(sameBox(roi.boxes[0], roi.boxes[1]), "new ROI replaces local edits");

    const Box first{0, 0, 500, 400}, second{500, 0, 900, 800};
    const Point anchor = paneFraction(first, {125, 300});
    const auto shift = zoomTranslation({.08, -.04}, anchor, 1.5);
    const auto checkAnchor = [&](Box pane, Point independentOffset) {
        const Point span{pane.width(), pane.height()};
        const Point relativeAnchor{span.x * (anchor.x - .5), span.y * (anchor.y - .5)};
        const Point before = independentOffset + Point{span.x * .08, span.y * -.04};
        const Point after = independentOffset * 1.5 + Point{span.x * shift.x, span.y * shift.y};
        const Point fixed = relativeAnchor + (before - relativeAnchor) * 1.5;
        require(close(after.x, fixed.x) && close(after.y, fixed.y), "pane-relative zoom with independent pan");
    };
    checkAnchor(first, {20, 70});
    checkAnchor(second, {-35, 30});
    const auto restored = zoomTranslation(shift, anchor, 1 / 1.5);
    require(close(restored.x, .08) && close(restored.y, -.04), "zoom round trip around same anchor");
}

void checkScreenshotPng(IWICImagingFactory* wic) {
    Raster screenshot{2, 2, {0xff112233, 0xff445566, 0xff778899, 0xffaabbcc}};
    const fs::path folder = L"build/fixtures";
    fs::path first, second;
    const auto firstResult = saveScreenshotPng(wic, screenshot, folder, first);
    const auto secondResult = saveScreenshotPng(wic, screenshot, folder, second);
    const auto decoded = decode(wic, first);
    std::error_code error;
    if (!first.empty()) fs::remove(first, error);
    if (!second.empty()) fs::remove(second, error);
    require(SUCCEEDED(firstResult) && SUCCEEDED(secondResult) && first != second,
        "screenshots save to distinct PNG paths");
    require(decoded->display && decoded->w == 2 && decoded->h == 2 &&
        decoded->display->bgra == screenshot.bgra, "screenshot PNG pixel round trip");
    fs::path failed;
    require(FAILED(saveScreenshotPng(wic, screenshot, folder / L"rgb16.png", failed)) && failed.empty(),
        "screenshot rejects a file as its output directory");
}

void checkLogic(IWICImagingFactory* wic) {
    const fs::path root = L"build/fixtures";
    const auto gray = decode(wic, root / L"gray16.png");
    require(gray->error.empty() && gray->bits == 16 && gray->gray && gray->w == 2, "16-bit PNG gray");
    require(gray->at(0, 0)->r == 30000 && gray->at(1, 0)->r == 30001, "16-bit gray native values");
    const auto rgb = decode(wic, root / L"rgb16.png");
    require(rgb->error.empty() && rgb->bits == 16 && !rgb->gray, "16-bit PNG RGB");
    require(rgb->at(0, 0)->g == 30001 && rgb->at(1, 0)->b == 30005, "16-bit RGB native values");
    require(thumbnail(wic, root / L"rgb16.png") != nullptr, "16-bit thumbnail");

    const auto grayTiff = decode(wic, root / L"gray16.tif");
    require(grayTiff->error.empty() && grayTiff->bits == 16 && grayTiff->gray, "16-bit TIFF gray");
    require(grayTiff->at(1, 1)->r == 30003, "16-bit TIFF gray native values");
    const auto rgbTiff = decode(wic, root / L"rgb16.tif");
    require(rgbTiff->error.empty() && rgbTiff->bits == 16 && !rgbTiff->gray, "16-bit TIFF RGB");
    require(rgbTiff->at(0, 0)->g == 30001 && rgbTiff->at(1, 0)->b == 30005, "16-bit TIFF RGB native values");

    const auto stats = measure(*gray, {}, {0, 0, 1, 1});
    require(stats.count == 4 && close(stats.rgb[0], 30001.5) && close(stats.sigma, std::sqrt(5. / 3.)),
        "ROI mean and sample standard deviation");
    require(!stats.hueValid && close(stats.hsv[1], 0.) && close(stats.lab[1], 0., .001), "gray HSV and Lab");

    Orientation orientation;
    orientation.apply(L'O');
    require(gray->oriented(0, 0, orientation)->r == 30002, "clockwise orientation");
    orientation.apply(L'I');
    require(gray->oriented(0, 0, orientation)->r == 30000, "mirror orientation");
    orientation.apply(L'U');
    require(gray->oriented(0, 0, orientation)->r == 30001, "flip orientation");
    const auto one = measure(*gray, orientation, {0, 0, .5, .5});
    require(one.count == 1 && close(one.rgb[0], 30001), "oriented ROI with pixel-edge bounds");

    const auto zero = difference(*gray, {}, *gray, {});
    require(zero.size() == 4 && close(zero[3], 0.), "zero signed difference");
    auto shown = differenceRaster(*gray, zero, 1);
    require(shown->bgra[0] == 0xff808080, "neutral difference display");
    Image brighter = *gray;
    ++brighter.pixels[0].r;
    ++brighter.pixels[0].g;
    ++brighter.pixels[0].b;
    const auto positive = difference(*gray, {}, brighter, {});
    require(close(positive[0], 1. / 257), "difference retains sub-8-bit signal");
    shown = differenceRaster(brighter, positive, 512);
    require(shown->bgra[0] == 0xff828282 && shown->bgra[1] == 0xff808080, "difference gain");
    Image smaller;
    smaller.w = 1;
    smaller.h = 1;
    smaller.pixels = {*gray->at(0, 0)};
    const auto overlap = difference(smaller, {}, *gray, {});
    require(close(overlap[0], 0.) && std::isnan(overlap[1]), "different dimensions only use pixel overlap");

    const fs::path directory = root / L"参考图";
    if (fs::exists(directory)) {
        for (const auto name : {L"frame-01.png", L"frame-02.jpg", L"frame-03.webp", L"frame-04.bmp",
                                L"frame-05.tif", L"frame-06.gif"}) {
            require(decode(wic, directory / name)->display != nullptr, "common format full decode");
            require(thumbnail(wic, directory / name) != nullptr, "common format thumbnail");
        }
        require(!decode(wic, directory / L"invalid.png")->error.empty(), "broken image handling");
    }
}
}

int main() {
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result{};
    try {
        ComPtr<IWICImagingFactory> wic;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic))), "WIC initialization");
        checkLogic(wic.Get());
        checkScreenshotPng(wic.Get());
        checkInteraction();
        puts("PASS: native 16-bit decode, thumbnails, common formats, ROI, orientation, signed difference, zoom, screenshot PNG.");
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return result;
}
