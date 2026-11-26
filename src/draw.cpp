// SPDX-License-Identifier: GPL-3.0-only
#include "draw.h"
#include <set>
#include <algorithm>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

static ID2D1Factory *factory;
static IDWriteFactory *dw;
static ID2D1DCRenderTarget *rt;
static ID2D1SolidColorBrush *brush;
static IDWriteTextFormat *formats[FONTS][4];
static float x0, y0, x1, y1; // what is painted, in 96-dpi units

static D2D1_COLOR_F color(Ink c) { return {(float)(c >> 16 & 255) / 255.f, (float)(c >> 8 & 255) / 255.f, (float)(c & 255) / 255.f, (float)(c >> 24) / 255.f}; }
static bool font_exists(const wchar_t *name)
{
    IDWriteFontCollection *c = nullptr;
    UINT32 i = 0;
    BOOL yes = FALSE;
    if (SUCCEEDED(dw->GetSystemFontCollection(&c, FALSE))) c->FindFamilyName(name, &i, &yes);
    release(c);
    return yes;
}
bool d_init()
{
    if (dw) return rt != nullptr;
    D2D1_FACTORY_OPTIONS o = {D2D1_DEBUG_LEVEL_NONE};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &o, (void **)&factory)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dw)))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES p = {};
    p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    p.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    if (FAILED(factory->CreateDCRenderTarget(&p, &rt))) return false;
    D2D1_COLOR_F black = {0, 0, 0, 1};
    rt->CreateSolidColorBrush(black, &brush);
    const wchar_t *text = font_exists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI", *big = font_exists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *icon = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    struct { const wchar_t *name; float size; DWRITE_FONT_WEIGHT weight; } l[FONTS] = {
        {text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL}, {text, 14, DWRITE_FONT_WEIGHT_NORMAL}, {text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {text, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {big, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD},
        {big, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {big, 44, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {icon, 16, DWRITE_FONT_WEIGHT_NORMAL}, {icon, 22, DWRITE_FONT_WEIGHT_NORMAL}};
    for (int f = 0; f < FONTS; f++)
        for (int a = 0; a < 4; a++)
        {
            if (FAILED(dw->CreateTextFormat(l[f].name, nullptr, l[f].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, l[f].size, L"ko-kr", &formats[f][a]))) return false;
            IDWriteTextFormat *t = formats[f][a];
            t->SetTextAlignment(a == A_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : a == A_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            t->SetParagraphAlignment(a == A_WRAP ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            t->SetWordWrapping(a == A_WRAP ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            if (a == A_WRAP) continue;
            IDWriteInlineObject *dots = nullptr;
            dw->CreateEllipsisTrimmingSign(t, &dots);
            DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            t->SetTrimming(&tr, dots);
            release(dots);
        }
    return true;
}
bool d_begin(HDC mem, const RECT &p, int dpi)
{
    if (!rt) return false;
    RECT r = {0, 0, p.right - p.left, p.bottom - p.top};
    if (FAILED(rt->BindDC(mem, &r))) return false;
    float k = 96.f / (float)dpi;
    rt->SetDpi((float)dpi, (float)dpi);
    rt->BeginDraw();
    D2D1_MATRIX_3X2_F m;
    m._11 = m._22 = 1;
    m._12 = m._21 = 0;
    m._31 = -(float)p.left * k;
    m._32 = -(float)p.top * k;
    rt->SetTransform(&m);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_COLOR_F none = {0, 0, 0, 0};
    rt->Clear(&none);
    x0 = (float)p.left * k;
    y0 = (float)p.top * k;
    x1 = (float)p.right * k;
    y1 = (float)p.bottom * k;
    return true;
}
void d_end() { rt->EndDraw(); }
bool d_visible(float x, float y, float w, float h) { return x < x1 && x + w > x0 && y < y1 && y + h > y0; }
static ID2D1SolidColorBrush *use(Ink c)
{
    D2D1_COLOR_F f = color(c);
    brush->SetColor(&f);
    return brush;
}
void d_text(Font f, Ink c, float x, float y, float w, float h, Align a, const std::wstring &s)
{
    if (s.empty() || w <= 0 || !d_visible(x, y, w, h)) return;
    static wchar_t v[8];
    static int test = GetEnvironmentVariableW(L"PKAPP_CLIP", v, 8) ? 1 : 0; // tests: says which texts do not fit (or fill more than so many percent)
    static double part = test && _wtoi(v) > 1 ? _wtoi(v) / 100.0 : 1;
    if (test)
    {
        static std::set<std::wstring> said;
        IDWriteTextLayout *l = nullptr;
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), formats[f][a == A_WRAP ? A_WRAP : 0], a == A_WRAP ? w : 4000, 4000, &l))) l->GetMetrics(&m);
        release(l);
        if ((a == A_WRAP ? m.height > h + 1 : m.width > w * (float)part + 0.5f) && said.insert(s + std::to_wstring((int)w)).second)
        {
            char u[700];
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u, 700, nullptr, nullptr);
            logf("clip: %s %.0f in %.0f: %s", a == A_WRAP ? "height" : "width", (double)(a == A_WRAP ? m.height : m.width), (double)(a == A_WRAP ? h : w), u);
        }
    }
    D2D1_RECT_F r = {x, y, x + w, y + h};
    rt->DrawText(s.c_str(), (UINT32)s.size(), formats[f][a], &r, use(c), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
float d_width(Font f, const std::wstring &s)
{
    IDWriteTextLayout *l = nullptr;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), formats[f][0], 4000, 100, &l))) l->GetMetrics(&m);
    release(l);
    return m.width;
}
float d_height(Font f, const std::wstring &s, float w)
{
    IDWriteTextLayout *l = nullptr;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), formats[f][A_WRAP], w, 4000, &l))) l->GetMetrics(&m);
    release(l);
    return m.height;
}
void d_box(float x, float y, float w, float h, float radius, Ink fill, Ink line, float width)
{
    if (!d_visible(x, y, w, h)) return;
    D2D1_ROUNDED_RECT r = {{x, y, x + w, y + h}, radius, radius};
    if (fill >> 24) rt->FillRoundedRectangle(&r, use(fill));
    if (!(line >> 24)) return;
    float k = width / 2;
    r.rect = {x + k, y + k, x + w - k, y + h - k};
    rt->DrawRoundedRectangle(&r, use(line), width, nullptr);
}
void d_fill(float x, float y, float w, float h, Ink c)
{
    if (!d_visible(x, y, w, h)) return;
    D2D1_RECT_F r = {x, y, x + w, y + h};
    rt->FillRectangle(&r, use(c));
}
void d_circle(float cx, float cy, float r, Ink fill, Ink line, float width)
{
    if (!d_visible(cx - r, cy - r, 2 * r, 2 * r)) return;
    D2D1_ELLIPSE e = {{cx, cy}, r, r};
    if (fill >> 24) rt->FillEllipse(&e, use(fill));
    if (line >> 24) rt->DrawEllipse(&e, use(line), width, nullptr);
}
void d_arc(float cx, float cy, float r, float part, Ink c, float width)
{
    if (part <= 0.002f || !d_visible(cx - r - width, cy - r - width, 2 * (r + width), 2 * (r + width))) return;
    if (part > 0.998f) { d_circle(cx, cy, r, 0, c, width); return; }
    ID2D1PathGeometry *g = nullptr;
    ID2D1GeometrySink *s = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s)))
    {
        float a = part * 6.2831853f;
        D2D1_POINT_2F p = {cx, cy - r};
        D2D1_ARC_SEGMENT e = {{cx + r * sinf(a), cy - r * cosf(a)}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, part > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
        s->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(&e);
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        static ID2D1StrokeStyle *round;
        if (!round)
        {
            D2D1_STROKE_STYLE_PROPERTIES sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
            factory->CreateStrokeStyle(&sp, nullptr, 0, &round);
        }
        rt->DrawGeometry(g, use(c), width, round);
    }
    release(s);
    release(g);
}
void d_glow(float cx, float cy, float r, Ink c)
{
    if (!d_visible(cx - r, cy - r, 2 * r, 2 * r)) return;
    D2D1_GRADIENT_STOP s[3] = {{0, color(c)}, {0.55f, color(fade(c, (int)(c >> 24) / 3))}, {1, color(fade(c, 0))}};
    ID2D1GradientStopCollection *g = nullptr;
    ID2D1RadialGradientBrush *b = nullptr;
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES p = {{cx, cy}, {0, 0}, r, r};
    if (SUCCEEDED(rt->CreateGradientStopCollection(s, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &g)) && SUCCEEDED(rt->CreateRadialGradientBrush(&p, nullptr, g, &b)))
    {
        D2D1_ELLIPSE e = {{cx, cy}, r, r};
        rt->FillEllipse(&e, b);
    }
    release(b);
    release(g);
}
void d_lines(const std::vector<std::pair<float, float>> &p, Ink c, float width, Ink under, float base)
{
    if (p.size() < 2) return;
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass == 0 && !(under >> 24)) continue;
        ID2D1PathGeometry *g = nullptr;
        ID2D1GeometrySink *s = nullptr;
        if (FAILED(factory->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { release(g); return; }
        D2D1_POINT_2F a = {p[0].first, p[0].second};
        s->BeginFigure(a, pass ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < p.size(); i++) { D2D1_POINT_2F q = {p[i].first, p[i].second}; s->AddLine(q); }
        if (!pass)
        {
            D2D1_POINT_2F q = {p.back().first, base}, z = {p[0].first, base};
            s->AddLine(q);
            s->AddLine(z);
        }
        s->EndFigure(pass ? D2D1_FIGURE_END_OPEN : D2D1_FIGURE_END_CLOSED);
        s->Close();
        if (pass) rt->DrawGeometry(g, use(c), width, nullptr);
        else rt->FillGeometry(g, use(under), nullptr);
        release(s);
        release(g);
    }
}
void d_clip(float x, float y, float w, float h)
{
    D2D1_RECT_F r = {x, y, x + w, y + h};
    rt->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_ALIASED);
}
void d_unclip() { rt->PopAxisAlignedClip(); }

// A picture that covers the rectangle; read once, kept small enough to be drawn quickly.
bool d_image(const std::wstring &path, float w, float h, float alpha)
{
    static std::wstring have;
    static ID2D1Bitmap *bm;
    if (path != have)
    {
        have = path;
        release(bm);
        IWICImagingFactory *wic = nullptr;
        IWICBitmapDecoder *dec = nullptr;
        IWICBitmapFrameDecode *fr = nullptr;
        IWICBitmapScaler *sc = nullptr;
        IWICFormatConverter *cv = nullptr;
        UINT iw = 0, ih = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&iw, &ih)) && iw && ih)
        {
            double k = std::min(1.0, 1600.0 / std::max(iw, ih));
            if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, std::max(1u, (UINT)(iw * k)), std::max(1u, (UINT)(ih * k)), WICBitmapInterpolationModeFant)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                rt->CreateBitmapFromWicBitmap(cv, nullptr, &bm);
        }
        release(cv);
        release(sc);
        release(fr);
        release(dec);
        release(wic);
    }
    if (!bm) return false;
    D2D1_SIZE_F s = bm->GetSize();
    float k = std::max(w / s.width, h / s.height), sw = w / k, sh = h / k;
    D2D1_RECT_F to = {0, 0, w, h}, from = {(s.width - sw) / 2, (s.height - sh) / 2, (s.width + sw) / 2, (s.height + sh) / 2};
    rt->DrawBitmap(bm, &to, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &from);
    return true;
}

void d_poly(const std::vector<std::pair<float, float>> &p, Ink c)
{
    ID2D1PathGeometry *g = nullptr;
    ID2D1GeometrySink *s = nullptr;
    if (p.size() > 2 && SUCCEEDED(factory->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s)))
    {
        D2D1_POINT_2F a = {p[0].first, p[0].second};
        s->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < p.size(); i++) { D2D1_POINT_2F q = {p[i].first, p[i].second}; s->AddLine(q); }
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        rt->FillGeometry(g, use(c), nullptr);
    }
    release(s);
    release(g);
}
