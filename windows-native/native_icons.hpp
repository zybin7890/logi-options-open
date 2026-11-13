#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <array>
#include <initializer_list>
#include <string_view>

inline void draw_native_icon(Gdiplus::Graphics& graphics, std::wstring_view name,
                             float left, float top, float size, COLORREF color) {
    using namespace Gdiplus;
    if (size <= 0.0f) return;
    const float scale = size / 24.0f;
    const auto x = [&](float value) { return left + value * scale; };
    const auto y = [&](float value) { return top + value * scale; };
    const auto point = [&](float cx, float cy) { return PointF(x(cx), y(cy)); };
    const Color ink(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Pen pen(ink, 1.55f * scale);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(ink);
    const auto state = graphics.Save();
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);

    const auto line = [&](float x1, float y1, float x2, float y2) {
        graphics.DrawLine(&pen, point(x1, y1), point(x2, y2));
    };
    const auto polyline = [&](std::initializer_list<PointF> vertices) {
        std::array<PointF, 8> path{};
        if (vertices.size() < 2 || vertices.size() > path.size()) return;
        auto out = path.begin();
        for (const auto& vertex : vertices) *out++ = point(vertex.X, vertex.Y);
        graphics.DrawLines(&pen, path.data(), static_cast<INT>(vertices.size()));
    };
    const auto circle = [&](float cx, float cy, float radius) {
        const float diameter = 2.0f * radius * scale;
        graphics.DrawEllipse(&pen, x(cx - radius), y(cy - radius), diameter, diameter);
    };
    const auto dot = [&](float cx, float cy, float radius) {
        const float diameter = 2.0f * radius * scale;
        graphics.FillEllipse(&brush, x(cx - radius), y(cy - radius), diameter, diameter);
    };
    const auto rounded = [&](float l, float t, float w, float h, float radius) {
        const float d = 2.0f * radius * scale;
        GraphicsPath outline;
        outline.AddArc(x(l), y(t), d, d, 180.0f, 90.0f);
        outline.AddArc(x(l + w - radius * 2.0f), y(t), d, d, 270.0f, 90.0f);
        outline.AddArc(x(l + w - radius * 2.0f), y(t + h - radius * 2.0f), d, d, 0.0f, 90.0f);
        outline.AddArc(x(l), y(t + h - radius * 2.0f), d, d, 90.0f, 90.0f);
        outline.CloseFigure();
        graphics.DrawPath(&pen, &outline);
    };

    if (name == L"back") {
        line(4.75f, 12.0f, 19.25f, 12.0f);
        polyline({{10.5f, 6.25f}, {4.75f, 12.0f}, {10.5f, 17.75f}});
    } else if (name == L"forward") {
        line(4.75f,12.0f,19.25f,12.0f);
        polyline({{13.5f,6.25f},{19.25f,12.0f},{13.5f,17.75f}});
    } else if (name == L"application") {
        rounded(3,4,18,16,3);
        line(3,9,21,9);
        circle(6,6.5f,0.5f);
    } else if (name == L"minimize") {
        line(5.5f, 12.0f, 18.5f, 12.0f);
    } else if (name == L"close") {
        line(6.25f, 6.25f, 17.75f, 17.75f);
        line(17.75f, 6.25f, 6.25f, 17.75f);
    } else if (name == L"buttons") {
        line(3.5f, 7.5f, 6.0f, 7.5f);
        circle(8.5f, 7.5f, 2.5f);
        line(11.0f, 7.5f, 20.5f, 7.5f);
        line(3.5f, 16.5f, 13.0f, 16.5f);
        circle(15.5f, 16.5f, 2.5f);
        line(18.0f, 16.5f, 20.5f, 16.5f);
    } else if (name == L"pointer") {
        rounded(6.25f, 2.65f, 11.5f, 18.7f, 5.75f);
        line(12.0f, 6.1f, 12.0f, 9.4f);
    } else if (name == L"easy") {
        rounded(2.1f, 8.0f, 4.9f, 7.0f, 0.9f);
        rounded(8.4f, 5.7f, 7.2f, 9.3f, 1.0f);
        rounded(17.0f, 8.0f, 4.9f, 7.0f, 0.9f);
        line(4.55f, 15.25f, 4.55f, 17.7f);
        line(12.0f, 15.25f, 12.0f, 17.7f);
        line(19.45f, 15.25f, 19.45f, 17.7f);
        line(2.75f, 18.2f, 6.35f, 18.2f);
        line(9.55f, 18.2f, 14.45f, 18.2f);
        line(17.65f, 18.2f, 21.25f, 18.2f);
    } else if (name == L"flow") {
        polyline({{8.1f, 13.2f}, {3.0f, 13.2f}, {3.0f, 4.6f},
                  {14.9f, 4.6f}, {14.9f, 8.7f}});
        rounded(8.6f, 9.0f, 12.4f, 8.8f, 1.1f);
        line(14.8f, 18.0f, 14.8f, 20.0f);
        line(11.8f, 20.5f, 17.8f, 20.5f);
    } else if (name == L"settings") {
        for (const float cy : {5.75f, 12.0f, 18.25f}) {
            dot(4.0f, cy, 0.95f);
            line(7.7f, cy, 20.0f, cy);
        }
    } else if (name == L"global") {
        circle(7.6f, 7.6f, 2.1f);
        circle(16.4f, 7.6f, 2.1f);
        circle(7.6f, 16.4f, 2.1f);
        circle(16.4f, 16.4f, 2.1f);
    } else if (name == L"add") {
        line(5.5f, 12.0f, 18.5f, 12.0f);
        line(12.0f, 5.5f, 12.0f, 18.5f);
    } else if (name == L"smart") {
        graphics.DrawArc(&pen, x(3.0f), y(3.0f), 18.0f * scale, 18.0f * scale, -16.0f, 303.0f);
        polyline({{7.4f, 12.2f}, {10.6f, 15.3f}, {17.2f, 7.8f}});
    } else if (name == L"ring") {
        constexpr float diagonal = 5.8f;
        for (const auto& p : {PointF(12.0f, 3.8f), PointF(12.0f, 20.2f),
                             PointF(3.8f, 12.0f), PointF(20.2f, 12.0f),
                             PointF(12.0f - diagonal, 12.0f - diagonal),
                             PointF(12.0f + diagonal, 12.0f - diagonal),
                             PointF(12.0f - diagonal, 12.0f + diagonal),
                             PointF(12.0f + diagonal, 12.0f + diagonal)})
            dot(p.X, p.Y, 1.15f);
    } else if (name == L"search") {
        circle(10.25f, 10.25f, 5.4f);
        line(14.15f, 14.15f, 19.7f, 19.7f);
    } else if (name == L"play") {
        polyline({{8.0f, 5.0f}, {18.0f, 12.0f}, {8.0f, 19.0f}, {8.0f, 5.0f}});
    } else if (name == L"grid") {
        for (float dx : {4.0f,14.0f}) for (float dy : {4.0f,14.0f}) rounded(dx,dy,6,6,0.5f);
    } else if (name == L"edit") {
        polyline({{5,17},{6,12},{16,2},{21,7},{11,17},{5,17}});
        line(14,4,19,9); line(5,21,20,21);
    } else if (name == L"note") {
        polyline({{15,4},{5,4},{5,21},{20,21},{20,14}});
        polyline({{10,12},{11,8},{18,1},{22,5},{15,12},{10,12}});
        line(9,16,16,16);
    } else if (name == L"keyboard") {
        rounded(2,5,20,14,2);
        for(float dx : {6.0f,10.0f,14.0f,18.0f}) {dot(dx,9,0.65f);dot(dx,12,0.65f);}
        line(7,16,17,16);
    } else if (name == L"desktop") {
        rounded(2,3,20,14,1.5f); line(12,17,12,21); line(7,21,17,21);
    } else if (name == L"lock") {
        rounded(5,10,14,12,2); graphics.DrawArc(&pen,x(8),y(2),8*scale,12*scale,180,180); dot(12,16,1);
    } else if (name == L"next" || name == L"previous") {
        if(name==L"previous") { graphics.TranslateTransform(left*2+size,0);graphics.ScaleTransform(-1,1); }
        polyline({{5,5},{15,12},{5,19},{5,5}});line(18,5,18,19);
    } else if (name == L"undo" || name == L"redo") {
        if(name==L"redo") { graphics.TranslateTransform(left*2+size,0);graphics.ScaleTransform(-1,1); }
        graphics.DrawArc(&pen,x(5),y(5),15*scale,15*scale,220,285);
        polyline({{4,3},{4,10},{11,10}});
    } else if (name == L"mute" || name == L"volume") {
        polyline({{4.5f, 9.0f}, {8.0f, 9.0f}, {12.5f, 5.0f}, {12.5f, 19.0f},
                  {8.0f, 15.0f}, {4.5f, 15.0f}, {4.5f, 9.0f}});
        if (name == L"mute") {
            line(16.5f, 10.0f, 20.5f, 14.0f);
            line(20.5f, 10.0f, 16.5f, 14.0f);
        } else {
            graphics.DrawArc(&pen,x(12),y(7),6*scale,10*scale,-70,140);
            graphics.DrawArc(&pen,x(12),y(4),10*scale,16*scale,-65,130);
        }
    } else if (name == L"capture") {
        rounded(3.5f, 6.25f, 17.0f, 12.5f, 2.0f);
        circle(12.0f, 12.5f, 3.25f);
        line(8.0f, 4.5f, 13.0f, 4.5f);
    } else if (name == L"folder") {
        polyline({{3.5f, 18.0f}, {3.5f, 6.25f}, {10.0f, 6.25f}, {12.0f, 8.5f},
                  {20.5f, 8.5f}, {20.5f, 18.0f}, {3.5f, 18.0f}});
    } else if (name == L"chevron") {
        polyline({{6.75f, 9.4f}, {12.0f, 14.6f}, {17.25f, 9.4f}});
    } else if (name == L"battery") {
        rounded(3.3f, 7.35f, 15.8f, 9.3f, 1.65f);
        graphics.FillRectangle(&brush, x(20.6f), y(10.1f), 1.65f * scale, 3.8f * scale);
        graphics.FillRectangle(&brush, x(5.7f), y(9.65f), 8.2f * scale, 4.7f * scale);
    } else if (name == L"bolt") {
        polyline({{13.8f, 2.9f}, {5.8f, 13.6f}, {11.1f, 13.6f}, {10.2f, 21.1f},
                  {18.4f, 10.4f}, {13.1f, 10.4f}, {13.8f, 2.9f}});
    } else if (name == L"chrome") {
        SolidBrush red(Color(255, 234, 67, 53)), green(Color(255, 52, 168, 83)),
                   yellow(Color(255, 251, 188, 5)), blue(Color(255, 66, 133, 244)),
                   edge(Color(255, 236, 245, 255));
        const float diameter = 18.0f * scale;
        graphics.FillPie(&red, x(3.0f), y(3.0f), diameter, diameter, 210.0f, 120.0f);
        graphics.FillPie(&yellow, x(3.0f), y(3.0f), diameter, diameter, 330.0f, 120.0f);
        graphics.FillPie(&green, x(3.0f), y(3.0f), diameter, diameter, 90.0f, 120.0f);
        graphics.FillEllipse(&edge, x(7.0f), y(7.0f), 10.0f * scale, 10.0f * scale);
        graphics.FillEllipse(&blue, x(7.9f), y(7.9f), 8.2f * scale, 8.2f * scale);
    } else if (name == L"firefox") {
        SolidBrush core(Color(255, 53, 71, 181)), flame(Color(255, 69, 150, 255)),
                   tip(Color(255, 138, 231, 255));
        graphics.FillEllipse(&core, x(3.0f), y(4.2f), 18.0f * scale, 17.5f * scale);
        GraphicsPath swirl;
        swirl.AddBezier(point(19.0f, 6.6f), point(23.1f, 10.5f), point(21.2f, 18.6f), point(16.9f, 20.7f));
        swirl.AddBezier(point(16.9f, 20.7f), point(11.1f, 24.1f), point(3.0f, 20.8f), point(2.4f, 14.8f));
        swirl.AddBezier(point(2.4f, 14.8f), point(2.0f, 10.5f), point(4.2f, 8.4f), point(4.9f, 5.3f));
        swirl.AddLine(point(4.9f, 5.3f), point(7.5f, 8.7f));
        swirl.AddBezier(point(7.5f, 8.7f), point(9.2f, 8.7f), point(10.3f, 9.7f), point(10.4f, 10.1f));
        swirl.AddBezier(point(10.4f, 10.1f), point(5.1f, 11.1f), point(5.4f, 17.7f), point(10.5f, 18.3f));
        swirl.AddBezier(point(10.5f, 18.3f), point(15.7f, 19.0f), point(18.0f, 14.0f), point(16.1f, 11.9f));
        swirl.AddBezier(point(16.1f, 11.9f), point(15.0f, 10.6f), point(13.6f, 10.6f), point(12.2f, 11.0f));
        swirl.AddBezier(point(12.2f, 11.0f), point(13.5f, 8.4f), point(15.7f, 7.8f), point(17.4f, 8.3f));
        swirl.AddLine(point(17.4f, 8.3f), point(16.3f, 3.0f));
        swirl.AddBezier(point(16.3f, 3.0f), point(18.0f, 4.3f), point(18.7f, 5.7f), point(19.0f, 6.6f));
        swirl.CloseFigure();
        graphics.FillPath(&flame, &swirl);
        GraphicsPath highlight;
        highlight.AddBezier(point(16.3f, 3.0f), point(17.9f, 3.7f), point(19.6f, 6.4f), point(20.1f, 8.7f));
        highlight.AddBezier(point(20.1f, 8.7f), point(17.9f, 7.9f), point(15.4f, 8.3f), point(12.2f, 11.0f));
        highlight.AddBezier(point(12.2f, 11.0f), point(12.5f, 9.0f), point(14.3f, 7.0f), point(16.8f, 6.7f));
        highlight.CloseFigure();
        graphics.FillPath(&tip, &highlight);
    }
    graphics.Restore(state);
}
