#include "ui/Icons.hpp"

#include <algorithm>

namespace chroma::ui {

void drawIcon(UiRenderer& r, Icon icon, const Rect& box, Color c) {
    const double k = std::min(box.w, box.h) / 24.0;
    const double ox = box.x + (box.w - 24 * k) / 2;
    const double oy = box.y + (box.h - 24 * k) / 2;
    const auto X = [&](double x) { return ox + x * k; };
    const auto Y = [&](double y) { return oy + y * k; };
    const auto line = [&](double x0, double y0, double x1, double y1, double w) { r.line(X(x0), Y(y0), X(x1), Y(y1), w * k, c); };
    const auto disc = [&](double x, double y, double radius) { r.disc(X(x), Y(y), radius * k, c); };
    const auto ring = [&](double x, double y, double radius, double w) { r.ring(X(x), Y(y), radius * k, w * k, c); };
    const auto box_ = [&](double x, double y, double w, double h, double radius) { r.fill({X(x), Y(y), w * k, h * k}, c, radius * k); };
    const auto frame = [&](double x, double y, double w, double h, double t, double radius) {
        r.stroke({X(x), Y(y), w * k, h * k}, c, t * k, radius * k);
    };

    switch (icon) {
    case Icon::Brush:
        line(19, 4, 11, 12.5, 2.4);   // handle
        line(11, 12.5, 6, 19, 4.6);   // bristles
        disc(5, 20, 1.6);
        break;
    case Icon::Eraser:
        line(8, 16, 16, 8, 7);        // body
        line(4, 20.5, 20, 20.5, 1.5); // the paper line
        break;
    case Icon::Smudge:
        line(9, 15, 16, 5, 4.6);      // finger
        disc(8.5, 16.5, 3.2);
        line(4, 21, 12, 21, 1.5);
        break;
    case Icon::Blur:
        ring(12, 14, 6.5, 1.8);
        line(12, 3.5, 12, 7.5, 1.8);
        disc(9.8, 13, 1.6);
        break;
    case Icon::Fill:
        frame(5, 9, 11, 11, 1.8, 2);  // bucket
        line(5, 9, 10.5, 3.5, 1.6);
        disc(19, 17, 2.2);            // drop
        line(19, 12.5, 19, 15, 1.6);
        break;
    case Icon::Select:
        for (int i = 0; i < 3; ++i) {
            const double a = 4 + i * 5.5;
            line(a, 5, a + 3, 5, 1.6);
            line(a, 19, a + 3, 19, 1.6);
        }
        for (int i = 0; i < 2; ++i) {
            const double a = 8 + i * 5.5;
            line(4, a, 4, a + 2.5, 1.6);
            line(20, a, 20, a + 2.5, 1.6);
        }
        break;
    case Icon::Vector:
        line(4, 19, 12, 9, 1.6);
        line(12, 9, 20, 5, 1.6);
        box_(2.5, 17.5, 4, 4, 0.6);
        box_(10, 7, 4, 4, 0.6);
        box_(18, 3, 4, 4, 0.6);
        break;
    case Icon::Transform:
        frame(6, 6, 12, 12, 1.5, 0);
        box_(4, 4, 4, 4, 0.5);
        box_(16, 4, 4, 4, 0.5);
        box_(4, 16, 4, 4, 0.5);
        box_(16, 16, 4, 4, 0.5);
        break;
    case Icon::Crop:
        line(7, 3, 7, 17, 1.8);
        line(7, 17, 21, 17, 1.8);
        line(3, 7, 17, 7, 1.8);
        line(17, 7, 17, 21, 1.8);
        break;
    case Icon::Pipette:
        line(5.5, 18.5, 15, 9, 2.4);
        disc(17.5, 6.5, 3.2);
        line(3.5, 20.5, 5.5, 18.5, 1.4);
        break;
    case Icon::Hand:
        disc(12, 15.5, 5.2);
        line(8.5, 13, 8.5, 6.5, 2.4);
        line(11.2, 12, 11.2, 4.5, 2.4);
        line(13.9, 12, 13.9, 5.2, 2.4);
        line(16.4, 13.5, 16.6, 8, 2.4);
        line(7.5, 16, 4.5, 12.5, 2.4);
        break;
    case Icon::Eye:
    case Icon::EyeOff:
        ring(12, 12, 7, 1.6);
        disc(12, 12, 2.6);
        if (icon == Icon::EyeOff) line(5, 19, 19, 5, 1.8);
        break;
    case Icon::Plus:
        line(12, 5, 12, 19, 1.8);
        line(5, 12, 19, 12, 1.8);
        break;
    case Icon::Duplicate:
        frame(8, 8, 11, 11, 1.6, 1.5);
        frame(5, 5, 11, 11, 1.6, 1.5);
        break;
    case Icon::Trash:
        frame(7, 8, 10, 12, 1.6, 1.5);
        line(5, 6, 19, 6, 1.6);
        line(10, 4, 14, 4, 1.6);
        break;
    case Icon::Folder:
        frame(4, 7, 16, 12, 1.6, 1.5);
        line(4.8, 7, 9, 4.5, 1.6);
        line(9, 4.5, 11.5, 7, 1.6);
        break;
    case Icon::Layers:
        line(4, 9, 12, 5, 1.6);
        line(12, 5, 20, 9, 1.6);
        line(20, 9, 12, 13, 1.6);
        line(12, 13, 4, 9, 1.6);
        line(4, 14, 12, 18, 1.6);
        line(12, 18, 20, 14, 1.6);
        break;
    case Icon::More:
        disc(6, 12, 1.8);
        disc(12, 12, 1.8);
        disc(18, 12, 1.8);
        break;
    case Icon::ChevronDown:
        line(7, 10, 12, 15, 1.8);
        line(12, 15, 17, 10, 1.8);
        break;
    case Icon::Check:
        line(5.5, 12.5, 10, 17, 2);
        line(10, 17, 19, 7, 2);
        break;
    case Icon::Lock:
        frame(6, 11, 12, 9, 1.6, 1.5);
        ring(12, 10, 4, 1.6);
        break;
    case Icon::Clip:
        line(8, 4, 8, 14, 1.6);
        line(8, 14, 13, 19, 1.6);
        line(13, 19, 20, 19, 1.6);
        break;
    case Icon::Search:
        ring(10.5, 10.5, 6, 1.8);
        line(15, 15, 20, 20, 2.2);
        break;
    }
}

}  // namespace chroma::ui
