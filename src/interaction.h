#pragma once

#include "image.h"
#include <algorithm>
#include <cmath>

namespace pic {

constexpr size_t maxCompare = 8;

enum RoiEdge : unsigned { RoiLeft = 1, RoiRight = 2, RoiTop = 4, RoiBottom = 8 };

struct Roi {
    explicit Roi(Box box) { boxes.fill(box); }
    std::array<Box, maxCompare> boxes;
};

inline Box adjustRoi(Box box, Point delta, Point minimum, unsigned edges) {
    if (!edges) {
        const double x = std::clamp(delta.x, -box.l, 1 - box.r);
        const double y = std::clamp(delta.y, -box.t, 1 - box.b);
        return {box.l + x, box.t + y, box.r + x, box.b + y};
    }
    const double minX = (std::min)(minimum.x, box.width());
    const double minY = (std::min)(minimum.y, box.height());
    if (edges & RoiLeft) box.l = std::clamp(box.l + delta.x, 0., box.r - minX);
    if (edges & RoiRight) box.r = std::clamp(box.r + delta.x, box.l + minX, 1.);
    if (edges & RoiTop) box.t = std::clamp(box.t + delta.y, 0., box.b - minY);
    if (edges & RoiBottom) box.b = std::clamp(box.b + delta.y, box.t + minY, 1.);
    return box;
}

inline Roi editRoi(const Roi& original, size_t source, size_t count, bool single, Point delta,
    const std::array<Point, maxCompare>& pixelSizes, unsigned edges) {
    Roi edited = original;
    const auto snap = [](double value, double pixel) { return std::round(value / pixel) * pixel; };
    const auto pixel = pixelSizes[source];
    delta = {snap(delta.x, pixel.x), snap(delta.y, pixel.y)};
    const size_t begin = single ? source : 0;
    const size_t end = single ? source + 1 : count;
    for (size_t slot = begin; slot < end; ++slot)
        edited.boxes[slot] = adjustRoi(original.boxes[slot], delta, pixelSizes[slot], edges);
    return edited;
}

inline Point paneFraction(Box pane, Point pointer) {
    return {(pointer.x - pane.l) / pane.width(), (pointer.y - pane.t) / pane.height()};
}

// Store wheel translation in pane fractions so the anchor works across pane sizes and DPI settings.
inline Point zoomTranslation(Point current, Point anchor, double factor) {
    return current * factor + (anchor - Point{.5, .5}) * (1 - factor);
}

template<class T>
bool moveBefore(std::vector<T>& items, size_t source, size_t boundary) {
    if (source >= items.size() || boundary > items.size() ||
        boundary == source || boundary == source + 1) return false;
    const auto first = items.begin();
    if (boundary < source) std::rotate(first + boundary, first + source, first + source + 1);
    else std::rotate(first + source, first + source + 1, first + boundary);
    return true;
}

} // namespace pic
