/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "LosslessCutStrip.h"
#include "LosslessCut.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "CMPCTheme.h"
#include "CMPCThemeUtil.h"
#include "PPageAccelTbl.h"
#include <algorithm>
#include <cmath>

using namespace Gdiplus;

// -------------------------------------------------------------------------------------------------
// Colors

namespace llcui
{
    void EnsureGdiplus()
    {
        static ULONG_PTR token = 0;
        if (!token) {
            GdiplusStartupInput input;
            GdiplusStartup(&token, &input, nullptr); // reference counted; intentionally kept for the process lifetime
        }
    }

    namespace
    {
        struct Hsl {
            double h, s, l; // 0..360, 0..100, 0..100 (color.js conventions)
        };

        Hsl ToHsl(COLORREF c)
        {
            const double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
            const double mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
            const double delta = mx - mn;
            Hsl out{ 0.0, 0.0, (mx + mn) / 2.0 };
            if (delta > 0.0) {
                out.s = out.l > 0.5 ? delta / (2.0 - mx - mn) : delta / (mx + mn);
                if (mx == r) {
                    out.h = (g - b) / delta + (g < b ? 6.0 : 0.0);
                } else if (mx == g) {
                    out.h = (b - r) / delta + 2.0;
                } else {
                    out.h = (r - g) / delta + 4.0;
                }
                out.h *= 60.0;
            }
            out.s *= 100.0;
            out.l *= 100.0;
            return out;
        }

        COLORREF FromHsl(Hsl hsl)
        {
            const double s = std::clamp(hsl.s, 0.0, 100.0) / 100.0, l = std::clamp(hsl.l, 0.0, 100.0) / 100.0;
            auto hue = [](double p, double q, double t) {
                if (t < 0) t += 1;
                if (t > 1) t -= 1;
                if (t < 1.0 / 6) return p + (q - p) * 6 * t;
                if (t < 1.0 / 2) return q;
                if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
                return p;
            };
            double r = l, g = l, b = l;
            if (s > 0) {
                const double h = hsl.h / 360.0;
                const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
                const double p = 2 * l - q;
                r = hue(p, q, h + 1.0 / 3);
                g = hue(p, q, h);
                b = hue(p, q, h - 1.0 / 3);
            }
            return RGB(std::lround(r * 255), std::lround(g * 255), std::lround(b * 255));
        }

        // LosslessCut's segment palette.
        const COLORREF kSegmentColors[] = {
            RGB(0xff, 0x51, 0x00), RGB(0xff, 0xc5, 0x69), RGB(0xdd, 0xff, 0xd1), RGB(0x00, 0xcc, 0xff), RGB(0xe9, 0xd1, 0xff),
            RGB(0xff, 0x00, 0x84), RGB(0xff, 0x69, 0x75), RGB(0xff, 0xe6, 0xd1), RGB(0xff, 0xff, 0x69), RGB(0x69, 0xff, 0x96),
            RGB(0x00, 0x8c, 0xff), RGB(0xae, 0x00, 0xff), RGB(0xff, 0x00, 0x2b), RGB(0xff, 0x8c, 0x00), RGB(0x8c, 0xff, 0x00),
            RGB(0x69, 0xff, 0xff), RGB(0x00, 0x44, 0xff), RGB(0xff, 0x00, 0xd4), RGB(0xff, 0xd1, 0xd9),
        };

        Hsl Base(int index)
        {
            const int n = _countof(kSegmentColors);
            return ToHsl(kSegmentColors[((index % n) + n) % n]);
        }

        // color.js: desaturate(r) => s -= s * r; saturate(r) => s += s * r; lightness(v) => l = v
        COLORREF Adjust(int index, double saturate, double lightness)
        {
            Hsl c = Base(index);
            c.s = std::clamp(c.s + c.s * saturate, 0.0, 100.0);
            c.l = lightness;
            return FromHsl(c);
        }

        int Luma(COLORREF c)
        {
            return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        }
    }

    COLORREF Blend(COLORREF fg, COLORREF bg, double alpha)
    {
        auto mix = [alpha](int f, int b) { return static_cast<int>(std::lround(f * alpha + b * (1.0 - alpha))); };
        return RGB(mix(GetRValue(fg), GetRValue(bg)), mix(GetGValue(fg), GetGValue(bg)), mix(GetBValue(fg), GetBValue(bg)));
    }

    Color ToGdi(COLORREF c, BYTE alpha)
    {
        return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    Palette GetPalette()
    {
        Palette p;
        if (AppIsThemeLoaded()) {
            p.bg = CMPCTheme::ContentBGColor;
            p.channel = CMPCTheme::ScrollBGColor;
            p.text = CMPCTheme::TextFGColor;
            p.textFade = CMPCTheme::TextFGColorFade;
            p.accent = CMPCTheme::SeekbarCurrentPositionColor;
        } else {
            p.bg = GetSysColor(COLOR_BTNFACE);
            p.channel = GetSysColor(COLOR_WINDOW);
            p.text = GetSysColor(COLOR_BTNTEXT);
            p.textFade = GetSysColor(COLOR_GRAYTEXT);
            p.accent = GetSysColor(COLOR_HIGHLIGHT);
        }
        p.dark = Luma(p.bg) < 128;
        p.textFade = Blend(p.text, p.bg, p.dark ? 0.62 : 0.68);
        p.textDisabled = Blend(p.text, p.bg, 0.30);
        p.hover = Blend(p.text, p.bg, p.dark ? 0.11 : 0.07);
        p.pressed = Blend(p.text, p.bg, p.dark ? 0.19 : 0.13);
        p.separator = Blend(p.text, p.bg, p.dark ? 0.16 : 0.18);
        p.keyframe = p.dark ? RGB(255, 200, 87) : RGB(204, 130, 0);
        p.ok = p.dark ? RGB(110, 205, 138) : RGB(26, 135, 60);
        p.warn = p.dark ? RGB(245, 178, 60) : RGB(190, 110, 0);
        p.error = p.dark ? RGB(255, 107, 107) : RGB(200, 30, 30);
        return p;
    }

    COLORREF SegmentFill(int index, SegState state, const Palette& pal)
    {
        // Same formulas LosslessCut uses for its timeline segments.
        if (pal.dark) {
            switch (state) {
                case SegState::Excluded: return Blend(Adjust(index, -0.3, 30), pal.channel, 0.5);
                case SegState::Current:  return Blend(Adjust(index, 0.2, 60), pal.channel, 0.7);
                default:                 return Blend(Adjust(index, -0.2, 50), pal.channel, 0.7);
            }
        }
        switch (state) {
            case SegState::Excluded: return Blend(Adjust(index, -0.3, 70), pal.channel, 0.5);
            case SegState::Current:  return Blend(Adjust(index, 0.2, 40), pal.channel, 0.8);
            default:                 return Blend(Adjust(index, 0.0, 35), pal.channel, 0.6);
        }
    }

    COLORREF SegmentEdge(int index, const Palette& pal)
    {
        return pal.dark ? Adjust(index, -0.1, 90) : Adjust(index, -0.2, 10);
    }

    COLORREF CutpointFill(int index, const Palette& pal)
    {
        return Adjust(index, -0.6, pal.dark ? 35 : 55);
    }

    COLORREF CutpointBorder(int index, const Palette& pal)
    {
        return Adjust(index, -0.6, pal.dark ? 45 : 35);
    }

    COLORREF SegmentDot(int index, const Palette& pal)
    {
        return Adjust(index, -0.2, pal.dark ? 60 : 45);
    }

    void AddRoundRect(GraphicsPath& path, const RectF& r, float radius)
    {
        const float d = std::min({ radius * 2.0f, r.Width, r.Height });
        if (d <= 0.5f) {
            path.AddRectangle(r);
            return;
        }
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
        path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
        path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
        path.CloseFigure();
    }
}

// -------------------------------------------------------------------------------------------------
// CLosslessCutStrip

namespace
{
    const LPCTSTR kLaunchLabel = _T("LosslessCut");
    const LPCTSTR kScopeLabels[] = { _T("All"), _T("Segments"), _T("Current") };
}

CLosslessCutStrip::CLosslessCutStrip(CMainFrame* pMainFrame)
    : m_pMainFrame(pMainFrame)
{
    const Btn order[] = {
        Btn::Launch, Btn::Info, Btn::PrevSegment, Btn::SetStart, Btn::PrevKeyframe, Btn::NextKeyframe, Btn::SetEnd, Btn::NextSegment,
        Btn::Add, Btn::Split, Btn::Remove, Btn::Undo, Btn::Redo, Btn::ScopeFull, Btn::ScopeSegments, Btn::ScopeSingle,
        Btn::Restrict, Btn::Loop, Btn::List, Btn::Status
    };
    for (Btn id : order) {
        Button b;
        b.id = id;
        m_buttons.push_back(b);
    }
}

BOOL CLosslessCutStrip::Create(CWnd* pParent)
{
    llcui::EnsureGdiplus();
    const CString cls = AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    if (!CWnd::CreateEx(0, cls, _T(""), WS_CHILD | WS_CLIPSIBLINGS, CRect(0, 0, 0, 0), pParent, 0)) {
        return FALSE;
    }
    m_tooltip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX);
    m_tooltip.SetMaxTipWidth(m_pMainFrame->m_dpi.ScaleX(420));
    m_tooltip.Activate(TRUE);
    return TRUE;
}

int CLosslessCutStrip::GetHeight() const
{
    return m_pMainFrame->m_dpi.ScaleY(32);
}

void CLosslessCutStrip::Refresh()
{
    if (m_hWnd && IsWindowVisible()) {
        Layout();
        Invalidate(FALSE);
    }
}

CLosslessCutStrip::Button* CLosslessCutStrip::Find(Btn id)
{
    for (auto& b : m_buttons) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

void CLosslessCutStrip::EnsureFonts()
{
    const int dpi = m_pMainFrame->m_dpi.ScaleY(96);
    if (m_font.GetSafeHandle() && m_fontDpi == dpi) {
        return;
    }
    m_font.DeleteObject();
    m_fontBold.DeleteObject();
    CMPCThemeUtil::getFontByType(m_font, this, CMPCThemeUtil::MessageFont);
    CMPCThemeUtil::getFontByType(m_fontBold, this, CMPCThemeUtil::MessageFont, false, true);
    m_fontDpi = dpi;
}

UINT CLosslessCutStrip::CommandFor(Btn id) const
{
    switch (id) {
        case Btn::Launch: return ID_LLC_LAUNCH;
        case Btn::PrevSegment: return ID_LLC_PREV_SEGMENT;
        case Btn::SetStart: return ID_LLC_SET_START;
        case Btn::PrevKeyframe: return ID_PLAY_SEEKKEYBACKWARD;
        case Btn::NextKeyframe: return ID_PLAY_SEEKKEYFORWARD;
        case Btn::SetEnd: return ID_LLC_SET_END;
        case Btn::NextSegment: return ID_LLC_NEXT_SEGMENT;
        case Btn::Add: return ID_LLC_ADD_SEGMENT;
        case Btn::Split: return ID_LLC_SPLIT_SEGMENT;
        case Btn::Remove: return ID_LLC_REMOVE_SEGMENT;
        case Btn::Undo: return ID_LLC_UNDO;
        case Btn::Redo: return ID_LLC_REDO;
        case Btn::ScopeFull: return ID_LLC_SCOPE_FULL;
        case Btn::ScopeSegments: return ID_LLC_SCOPE_SEGMENTS;
        case Btn::ScopeSingle: return ID_LLC_SCOPE_SINGLE;
        case Btn::Restrict: return ID_LLC_TOGGLE_RESTRICT;
        case Btn::Loop: return ID_LLC_TOGGLE_LOOP;
        case Btn::List: return ID_LLC_TOGGLE_SEGMENT_LIST;
        default: return 0;
    }
}

CString CLosslessCutStrip::InfoText(CString& detail) const
{
    const auto& llc = m_pMainFrame->m_llc;
    const auto& segs = llc.GetSegments();
    detail.Empty();
    if (segs.empty()) {
        detail = _T("set a start and an end to make one");
        return _T("No segments");
    }
    const int cur = llc.GetCurrentIndex();
    CString title;
    if (cur < 0 || cur >= static_cast<int>(segs.size())) {
        REFERENCE_TIME total = 0;
        for (const auto& s : segs) {
            if (s.selected && s.end) {
                total += CLosslessCutController::ToRT(*s.end) - CLosslessCutController::ToRT(s.start);
            }
        }
        title.Format(_T("%d segments"), static_cast<int>(segs.size()));
        detail = _T("total ") + CLosslessCutController::FormatTime(total);
        return title;
    }
    const auto& seg = segs[cur];
    title.Format(_T("Segment %d / %d"), cur + 1, static_cast<int>(segs.size()));
    const REFERENCE_TIME start = CLosslessCutController::ToRT(seg.start);
    if (seg.IsMarker()) {
        detail = CLosslessCutController::FormatTime(start) + _T("  \x2192  set the end");
    } else {
        const REFERENCE_TIME end = CLosslessCutController::ToRT(*seg.end);
        const REFERENCE_TIME len = end - start;
        CString length;
        if (len < 600000000LL) {
            length.Format(_T("%.2f s"), len / 10000000.0);
        } else {
            length = CLosslessCutController::FormatTime(len);
        }
        detail = CLosslessCutController::FormatTime(start) + _T(" \x2013 ") + CLosslessCutController::FormatTime(end) + _T("   ") + length;
    }
    if (!seg.name.empty()) {
        detail = CString(llc::Utf8ToWide(seg.name).c_str()) + _T("   ") + detail;
    }
    if (!seg.selected) {
        detail += _T("   (excluded)");
    }
    return title;
}

void CLosslessCutStrip::Layout()
{
    EnsureFonts();
    CRect client;
    GetClientRect(client);
    const auto& dpi = m_pMainFrame->m_dpi;
    const int W = client.Width();
    const int bh = dpi.ScaleY(24);
    const int top = (client.Height() - bh) / 2;
    const int pad = dpi.ScaleX(6);
    const int padLeft = dpi.ScaleX(3);
    const int gap = dpi.ScaleX(2);
    const int groupGap = dpi.ScaleX(14);
    const int iconW = dpi.ScaleX(28);
    const int pillW = dpi.ScaleX(36);

    CClientDC dc(this);
    CFont* oldFont = dc.SelectObject(&m_fontBold);
    const int launchTextW = dc.GetTextExtent(kLaunchLabel).cx;
    dc.SelectObject(&m_font);
    int scopeW[3];
    for (int i = 0; i < 3; i++) {
        scopeW[i] = dc.GetTextExtent(kScopeLabels[i]).cx + dpi.ScaleX(18);
    }
    dc.SelectObject(oldFont);

    // Degrade gracefully on narrow windows: drop the least important things first.
    bool showUndo = true, showLaunchText = true, showEditGroup = true, showToggles = true, showScope = true;
    // The launch button is laid out by its content (icon, gap, label) so it sits flush with the
    // edge and its neighbours; its hover box bleeds outward into the surrounding space instead.
    const int bleed = dpi.ScaleX(7);
    const int launchContentW = dpi.ScaleX(16) + dpi.ScaleX(6) + launchTextW;
    auto widthOf = [&](bool withInfoMin) {
        const int launch = showLaunchText ? launchContentW + 2 * bleed : iconW;
        const int left = launch + (withInfoMin ? groupGap + dpi.ScaleX(150) : 0);
        const int center = 4 * iconW + 2 * pillW + 5 * gap;
        int right = 2 * iconW + gap; // list + status
        if (showEditGroup) {
            right += 3 * iconW + 2 * gap + groupGap;
        }
        if (showUndo) {
            right += 2 * iconW + gap + groupGap;
        }
        if (showScope) {
            right += scopeW[0] + scopeW[1] + scopeW[2] + groupGap;
        }
        if (showToggles) {
            right += 2 * iconW + gap + groupGap;
        }
        return padLeft + pad + left + center + right + 2 * groupGap;
    };
    bool* degrade[] = { &showUndo, &showLaunchText, &showToggles, &showEditGroup, &showScope };
    for (bool* flag : degrade) {
        if (widthOf(false) <= W) {
            break;
        }
        *flag = false;
    }

    for (auto& b : m_buttons) {
        b.visible = false;
        b.rc.SetRectEmpty();
    }
    m_separators.clear();
    m_infoRect.SetRectEmpty();

    auto place = [&](Btn id, int x, int w) {
        Button* b = Find(id);
        b->rc = CRect(x, top, x + w, top + bh);
        b->visible = true;
        return x + w;
    };
    auto separatorAt = [&](int x) {
        m_separators.emplace_back(x, top + dpi.ScaleY(5), x + 1, top + bh - dpi.ScaleY(5));
    };

    // Left: launch button (its icon lines up with the toolbar's first button), then the current segment.
    m_launchBleed = showLaunchText ? bleed : 0;
    int x = padLeft + m_launchBleed;
    x = place(Btn::Launch, x, showLaunchText ? launchContentW : iconW);
    const int leftEnd = x + m_launchBleed;

    // Right, from the right edge inwards.
    int rx = W - pad;
    auto placeRight = [&](Btn id, int w) {
        rx -= w;
        place(id, rx, w);
        rx -= gap;
    };
    placeRight(Btn::Status, iconW);
    placeRight(Btn::List, iconW);
    if (showToggles) {
        rx -= groupGap - gap;
        separatorAt(rx + groupGap / 2);
        placeRight(Btn::Loop, iconW);
        placeRight(Btn::Restrict, iconW);
    }
    if (showScope) {
        rx -= groupGap - gap;
        if (!showToggles) {
            separatorAt(rx + groupGap / 2);
        }
        rx += gap;
        for (int i = 2; i >= 0; i--) {
            rx -= scopeW[i];
            place(i == 0 ? Btn::ScopeFull : i == 1 ? Btn::ScopeSegments : Btn::ScopeSingle, rx, scopeW[i]);
        }
        rx -= gap;
    }
    if (showUndo) {
        rx -= groupGap - gap;
        separatorAt(rx + groupGap / 2);
        placeRight(Btn::Redo, iconW);
        placeRight(Btn::Undo, iconW);
    }
    if (showEditGroup) {
        rx -= groupGap - gap;
        separatorAt(rx + groupGap / 2);
        placeRight(Btn::Remove, iconW);
        placeRight(Btn::Split, iconW);
        placeRight(Btn::Add, iconW);
    }
    const int rightStart = rx + gap;

    // Center: the cut point and keyframe controls, centered on the bar like in LosslessCut.
    const int centerW = 4 * iconW + 2 * pillW + 5 * gap;
    int cx = (W - centerW) / 2;
    cx = std::min(cx, rightStart - groupGap - centerW);
    cx = std::max(cx, leftEnd + groupGap);
    const Btn centerOrder[] = { Btn::PrevSegment, Btn::SetStart, Btn::PrevKeyframe, Btn::NextKeyframe, Btn::SetEnd, Btn::NextSegment };
    for (Btn id : centerOrder) {
        const int w = (id == Btn::SetStart || id == Btn::SetEnd) ? pillW : iconW;
        cx = place(id, cx, w) + gap;
    }
    const int centerStart = Find(Btn::PrevSegment)->rc.left;

    // The segment info takes the room left between the launch button and the center group.
    const int infoLeft = leftEnd + groupGap;
    const int infoRight = centerStart - groupGap;
    if (infoRight - infoLeft >= dpi.ScaleX(90)) {
        separatorAt(leftEnd + groupGap / 2);
        place(Btn::Info, infoLeft, infoRight - infoLeft);
        m_infoRect = Find(Btn::Info)->rc;
    }

    UpdateTooltips();
}

void CLosslessCutStrip::UpdateStates()
{
    const auto& llc = m_pMainFrame->m_llc;
    const bool loaded = m_pMainFrame->GetLoadState() == MLS::LOADED;
    const bool edit = llc.CanEdit() && loaded;
    const bool haveSegs = llc.IsActive() && !llc.GetSegments().empty();
    const auto scope = llc.GetScope();

    for (auto& b : m_buttons) {
        b.enabled = true;
        b.active = false;
        switch (b.id) {
            case Btn::PrevSegment:
            case Btn::NextSegment:
                b.enabled = haveSegs && loaded;
                break;
            case Btn::SetStart:
            case Btn::SetEnd:
            case Btn::Add:
                b.enabled = edit;
                break;
            case Btn::Split:
                b.enabled = edit && llc.SegmentAt(m_pMainFrame->GetPos()) >= 0;
                break;
            case Btn::Remove:
                b.enabled = edit && llc.GetCurrentSegment() != nullptr;
                break;
            case Btn::PrevKeyframe:
            case Btn::NextKeyframe:
                b.enabled = loaded && llc.HasKeyframes();
                b.active = b.enabled && llc.IsOnKeyframe();
                break;
            case Btn::Undo:
                b.enabled = llc.CanUndo();
                break;
            case Btn::Redo:
                b.enabled = llc.CanRedo();
                break;
            case Btn::ScopeFull:
                b.active = scope == CLosslessCutController::Scope::Full;
                break;
            case Btn::ScopeSegments:
                b.active = scope == CLosslessCutController::Scope::Segments;
                break;
            case Btn::ScopeSingle:
                b.active = scope == CLosslessCutController::Scope::Single;
                break;
            case Btn::Restrict:
                b.active = llc.IsRestricted();
                break;
            case Btn::Loop:
                b.active = llc.IsLooping();
                break;
            case Btn::List:
                b.enabled = llc.IsActive();
                b.active = llc.IsSegmentListShown();
                break;
            case Btn::Info:
                b.enabled = false; // not clickable
                break;
            default:
                break;
        }
    }
}

void CLosslessCutStrip::UpdateTooltips()
{
    if (!m_tooltip.m_hWnd) {
        return;
    }
    // MFC's CToolTipCtrl::AddTool(rect) leaves out TTF_SUBCLASS, so the tooltip would never see
    // the mouse. Register the tools directly, and only move existing ones: re-adding them on
    // every refresh would restart the hover delay while the video plays.
    for (UINT_PTR i = 0; i < m_buttons.size(); i++) {
        TOOLINFO ti = { sizeof(TOOLINFO) };
        ti.hwnd = m_hWnd;
        ti.uId = i + 1;
        const bool exists = !!m_tooltip.SendMessage(TTM_GETTOOLINFO, 0, reinterpret_cast<LPARAM>(&ti));
        if (m_buttons[i].visible) {
            ti.rect = HitRect(m_buttons[i]);
            if (exists) {
                m_tooltip.SendMessage(TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
            } else {
                ti.uFlags = TTF_SUBCLASS;
                ti.lpszText = LPSTR_TEXTCALLBACK;
                m_tooltip.SendMessage(TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
            }
        } else if (exists) {
            m_tooltip.SendMessage(TTM_DELTOOL, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
}

CString CLosslessCutStrip::TooltipFor(const Button& b) const
{
    const auto& llc = m_pMainFrame->m_llc;
    CString text;
    switch (b.id) {
        case Btn::Launch: text = _T("Open this video in LosslessCut, for exporting"); break;
        case Btn::Info: {
            CString detail;
            text = InfoText(detail);
            if (!detail.IsEmpty()) {
                text += _T("\n") + detail;
            }
            text += _T("\nRight-click the timeline or this bar for more");
            return text;
        }
        case Btn::PrevSegment: text = _T("Previous segment"); break;
        case Btn::SetStart: text = _T("Set the segment start at the playhead"); break;
        case Btn::PrevKeyframe: text = _T("Previous keyframe"); break;
        case Btn::NextKeyframe: text = _T("Next keyframe"); break;
        case Btn::SetEnd: text = _T("Set the segment end at the playhead"); break;
        case Btn::NextSegment: text = _T("Next segment"); break;
        case Btn::Add: text = _T("Add a segment at the playhead"); break;
        case Btn::Split: text = _T("Split the segment at the playhead"); break;
        case Btn::Remove: text = _T("Remove the current segment"); break;
        case Btn::Undo: text = _T("Undo"); break;
        case Btn::Redo: text = _T("Redo"); break;
        case Btn::ScopeFull: text = _T("Play everything"); break;
        case Btn::ScopeSegments: text = _T("Play only the segments included in the export, skipping the gaps"); break;
        case Btn::ScopeSingle: text = _T("Play only the current segment"); break;
        case Btn::Restrict: text = _T("Keep seeking inside the segments (dragging, mouse wheel and keys)"); break;
        case Btn::Loop: text = _T("Loop: start over after the last segment instead of pausing"); break;
        case Btn::List: text = b.active ? _T("Hide the segment list") : _T("Show the segment list"); break;
        case Btn::Status: return llc.GetStatusText() + _T("\nClick to show the project file");
    }
    const UINT cmd = CommandFor(b.id);
    if (b.id == Btn::SetStart || b.id == Btn::SetEnd) {
        text += b.id == Btn::SetStart ? _T("  (I)") : _T("  (O)"); // fixed keys in LosslessCut mode
    } else if (cmd) {
        const CString keys = CPPageAccelTbl::MakeAccelShortcutLabel(cmd);
        if (!keys.IsEmpty()) {
            text += _T("  (") + keys + _T(")");
        }
    }
    if ((b.id == Btn::PrevKeyframe || b.id == Btn::NextKeyframe)) {
        if (!b.enabled) {
            text += _T("\nThis file reports no keyframes");
        } else if (b.active) {
            text += _T("\nThe playhead is on a keyframe");
        }
    }
    return text;
}

CRect CLosslessCutStrip::HitRect(const Button& b) const
{
    CRect r(b.rc);
    if (b.id == Btn::Launch) {
        r.InflateRect(m_launchBleed, 0);
    }
    return r;
}

int CLosslessCutStrip::HitTest(CPoint pt) const
{
    for (int i = 0; i < static_cast<int>(m_buttons.size()); i++) {
        if (m_buttons[i].visible && HitRect(m_buttons[i]).PtInRect(pt)) {
            return i;
        }
    }
    return -1;
}

void CLosslessCutStrip::Execute(const Button& b)
{
    if (b.id == Btn::Status) {
        const CString path = m_pMainFrame->m_llc.GetProjectPath();
        if (!path.IsEmpty() && PathFileExists(path)) {
            ShellExecute(nullptr, _T("open"), _T("explorer.exe"), _T("/select,\"") + path + _T("\""), nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    if (const UINT cmd = CommandFor(b.id)) {
        m_pMainFrame->SendMessage(WM_COMMAND, cmd);
    }
}

// -------------------------------------------------------------------------------------------------
// Painting

void CLosslessCutStrip::DrawIcon(Graphics& g, Btn id, const RectF& box, const Color& color, const llcui::Palette& pal)
{
    const float k = box.Width / 16.0f;
    auto P = [&](float x, float y) { return PointF(box.X + x * k, box.Y + y * k); };
    Pen pen(color, 1.6f * k);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush brush(color);

    auto key = [&](bool mirror) {
        auto X = [&](float x) { return mirror ? 16.0f - x : x; };
        // chevron
        PointF chev[] = { P(X(4.2f), 4.6f), P(X(1.4f), 8.0f), P(X(4.2f), 11.4f) };
        g.DrawLines(&pen, chev, 3);
        // bow
        const float cx = X(8.2f), r = 2.6f;
        g.DrawEllipse(&pen, box.X + (cx - r) * k, box.Y + (8.0f - r) * k, 2 * r * k, 2 * r * k);
        // shaft and teeth
        PointF shaft[] = { P(X(10.8f), 8.0f), P(X(15.2f), 8.0f), P(X(15.2f), 10.6f) };
        g.DrawLines(&pen, shaft, 3);
        g.DrawLine(&pen, P(X(13.2f), 8.0f), P(X(13.2f), 10.0f));
    };

    switch (id) {
        case Btn::Launch: {
            const float r = 2.2f;
            g.DrawEllipse(&pen, box.X + (4.4f - r) * k, box.Y + (12.0f - r) * k, 2 * r * k, 2 * r * k);
            g.DrawEllipse(&pen, box.X + (11.6f - r) * k, box.Y + (12.0f - r) * k, 2 * r * k, 2 * r * k);
            g.DrawLine(&pen, P(5.9f, 10.3f), P(12.2f, 1.8f));
            g.DrawLine(&pen, P(10.1f, 10.3f), P(3.8f, 1.8f));
            break;
        }
        case Btn::PrevSegment: {
            g.FillRectangle(&brush, box.X + 2.6f * k, box.Y + 3.2f * k, 2.0f * k, 9.6f * k);
            PointF tri[] = { P(13.2f, 3.2f), P(13.2f, 12.8f), P(6.0f, 8.0f) };
            g.FillPolygon(&brush, tri, 3);
            break;
        }
        case Btn::NextSegment: {
            g.FillRectangle(&brush, box.X + 11.4f * k, box.Y + 3.2f * k, 2.0f * k, 9.6f * k);
            PointF tri[] = { P(2.8f, 3.2f), P(2.8f, 12.8f), P(10.0f, 8.0f) };
            g.FillPolygon(&brush, tri, 3);
            break;
        }
        case Btn::SetStart: {
            Pen thick(color, 2.0f * k);
            thick.SetLineJoin(LineJoinMiter);
            PointF br[] = { P(8.5f, 2.2f), P(4.4f, 2.2f), P(4.4f, 13.8f), P(8.5f, 13.8f) };
            g.DrawLines(&thick, br, 4);
            PointF tri[] = { P(8.6f, 5.0f), P(8.6f, 11.0f), P(12.8f, 8.0f) };
            g.FillPolygon(&brush, tri, 3);
            break;
        }
        case Btn::SetEnd: {
            Pen thick(color, 2.0f * k);
            thick.SetLineJoin(LineJoinMiter);
            PointF br[] = { P(7.5f, 2.2f), P(11.6f, 2.2f), P(11.6f, 13.8f), P(7.5f, 13.8f) };
            g.DrawLines(&thick, br, 4);
            PointF tri[] = { P(7.4f, 5.0f), P(7.4f, 11.0f), P(3.2f, 8.0f) };
            g.FillPolygon(&brush, tri, 3);
            break;
        }
        case Btn::PrevKeyframe:
            key(false);
            break;
        case Btn::NextKeyframe:
            key(true);
            break;
        case Btn::Add:
            g.DrawLine(&pen, P(8.0f, 3.2f), P(8.0f, 12.8f));
            g.DrawLine(&pen, P(3.2f, 8.0f), P(12.8f, 8.0f));
            break;
        case Btn::Split: {
            GraphicsPath a, b;
            llcui::AddRoundRect(a, RectF(box.X + 1.6f * k, box.Y + 5.0f * k, 4.6f * k, 6.0f * k), 1.2f * k);
            llcui::AddRoundRect(b, RectF(box.X + 9.8f * k, box.Y + 5.0f * k, 4.6f * k, 6.0f * k), 1.2f * k);
            g.DrawPath(&pen, &a);
            g.DrawPath(&pen, &b);
            Pen dash(color, 1.4f * k);
            dash.SetDashStyle(DashStyleDash);
            g.DrawLine(&dash, P(8.0f, 1.6f), P(8.0f, 14.4f));
            break;
        }
        case Btn::Remove: {
            g.DrawLine(&pen, P(2.8f, 4.4f), P(13.2f, 4.4f));
            PointF handle[] = { P(6.2f, 4.4f), P(6.6f, 2.4f), P(9.4f, 2.4f), P(9.8f, 4.4f) };
            g.DrawLines(&pen, handle, 4);
            PointF body[] = { P(4.2f, 4.4f), P(5.1f, 13.6f), P(10.9f, 13.6f), P(11.8f, 4.4f) };
            g.DrawLines(&pen, body, 4);
            Pen thin(color, 1.2f * k);
            g.DrawLine(&thin, P(6.9f, 7.0f), P(7.1f, 11.2f));
            g.DrawLine(&thin, P(9.1f, 7.0f), P(8.9f, 11.2f));
            break;
        }
        case Btn::Undo:
        case Btn::Redo: {
            const GraphicsState state = g.Save();
            if (id == Btn::Redo) {
                // mirror around the icon's vertical center line
                g.TranslateTransform(2 * box.X + box.Width, 0);
                g.ScaleTransform(-1, 1);
            }
            GraphicsPath path;
            path.AddLine(P(3.4f, 6.2f), P(9.6f, 6.2f));
            path.AddArc(box.X + 6.0f * k, box.Y + 6.2f * k, 7.2f * k, 7.2f * k, 270.0f, 180.0f);
            path.AddLine(P(9.6f, 13.4f), P(5.6f, 13.4f));
            g.DrawPath(&pen, &path);
            PointF head[] = { P(6.0f, 3.4f), P(3.2f, 6.2f), P(6.0f, 9.0f) };
            g.DrawLines(&pen, head, 3);
            g.Restore(state);
            break;
        }
        case Btn::Restrict: {
            GraphicsPath body;
            llcui::AddRoundRect(body, RectF(box.X + 3.2f * k, box.Y + 7.0f * k, 9.6f * k, 7.2f * k), 1.6f * k);
            g.FillPath(&brush, &body);
            GraphicsPath shackle;
            shackle.AddLine(P(5.4f, 7.2f), P(5.4f, 5.2f));
            shackle.AddArc(box.X + 5.4f * k, box.Y + 2.4f * k, 5.2f * k, 5.6f * k, 180.0f, 180.0f);
            shackle.AddLine(P(10.6f, 5.2f), P(10.6f, 7.2f));
            g.DrawPath(&pen, &shackle);
            break;
        }
        case Btn::Loop: {
            GraphicsPath path;
            path.AddArc(box.X + 2.0f * k, box.Y + 4.0f * k, 6.0f * k, 8.0f * k, 90.0f, 180.0f);
            path.AddLine(P(5.0f, 4.0f), P(11.0f, 4.0f));
            path.AddArc(box.X + 8.0f * k, box.Y + 4.0f * k, 6.0f * k, 8.0f * k, 270.0f, 180.0f);
            path.AddLine(P(11.0f, 12.0f), P(7.0f, 12.0f));
            g.DrawPath(&pen, &path);
            PointF head[] = { P(8.6f, 2.0f), P(11.0f, 4.0f), P(8.6f, 6.0f) };
            g.DrawLines(&pen, head, 3);
            break;
        }
        case Btn::List: {
            GraphicsPath frame;
            llcui::AddRoundRect(frame, RectF(box.X + 1.8f * k, box.Y + 2.8f * k, 12.4f * k, 10.4f * k), 2.0f * k);
            g.DrawPath(&pen, &frame);
            g.DrawLine(&pen, P(9.4f, 2.8f), P(9.4f, 13.2f));
            Pen thin(color, 1.2f * k);
            thin.SetStartCap(LineCapRound);
            thin.SetEndCap(LineCapRound);
            for (float y : { 5.8f, 8.0f, 10.2f }) {
                g.DrawLine(&thin, P(11.0f, y), P(12.6f, y));
            }
            break;
        }
        case Btn::Status: {
            switch (m_pMainFrame->m_llc.GetStatus()) {
                case CLosslessCutController::Status::Saved: {
                    Pen p(llcui::ToGdi(pal.ok), 1.8f * k);
                    p.SetLineJoin(LineJoinRound);
                    p.SetStartCap(LineCapRound);
                    p.SetEndCap(LineCapRound);
                    PointF check[] = { P(3.6f, 8.6f), P(6.6f, 11.4f), P(12.4f, 4.8f) };
                    g.DrawLines(&p, check, 3);
                    break;
                }
                case CLosslessCutController::Status::Error: {
                    Pen p(llcui::ToGdi(pal.error), 1.6f * k);
                    p.SetLineJoin(LineJoinRound);
                    PointF tri[] = { P(8.0f, 2.2f), P(14.4f, 13.6f), P(1.6f, 13.6f) };
                    g.DrawPolygon(&p, tri, 3);
                    SolidBrush b(llcui::ToGdi(pal.error));
                    g.FillRectangle(&b, box.X + 7.25f * k, box.Y + 6.0f * k, 1.5f * k, 4.2f * k);
                    g.FillEllipse(&b, box.X + 7.1f * k, box.Y + 11.0f * k, 1.8f * k, 1.8f * k);
                    break;
                }
                case CLosslessCutController::Status::ReadOnly:
                    DrawIcon(g, Btn::Restrict, box, llcui::ToGdi(pal.warn), pal);
                    break;
                case CLosslessCutController::Status::NotSavedYet: {
                    Pen p(color, 1.4f * k);
                    p.SetDashStyle(DashStyleDot);
                    g.DrawEllipse(&p, box.X + 3.0f * k, box.Y + 3.0f * k, 10.0f * k, 10.0f * k);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
}

void CLosslessCutStrip::DrawButton(Graphics& g, CDC& dc, const Button& b, int index, const llcui::Palette& pal)
{
    const auto& dpi = m_pMainFrame->m_dpi;
    const bool hot = (index == m_hot) && b.enabled;
    const bool pressed = (index == m_pressed) && hot;
    const CRect box = HitRect(b); // the hover/pressed background; wider than the content for the launch button
    const RectF rf(static_cast<REAL>(box.left), static_cast<REAL>(box.top), static_cast<REAL>(box.Width()), static_cast<REAL>(box.Height()));
    const float radius = static_cast<float>(dpi.ScaleX(6));
    const float iconSize = static_cast<float>(dpi.ScaleX(16));
    RectF iconBox(rf.X + (rf.Width - iconSize) / 2, rf.Y + (rf.Height - iconSize) / 2, iconSize, iconSize);

    if (b.id == Btn::Info) {
        CString detail;
        const CString title = InfoText(detail);
        const auto& llc = m_pMainFrame->m_llc;
        int x = b.rc.left;
        const int cur = llc.GetCurrentIndex();
        if (cur >= 0 && cur < static_cast<int>(llc.GetSegments().size())) {
            const float d = static_cast<float>(dpi.ScaleX(8));
            SolidBrush dot(llcui::ToGdi(llcui::SegmentDot(cur, pal)));
            g.FillEllipse(&dot, static_cast<REAL>(x), rf.Y + (rf.Height - d) / 2, d, d);
            x += dpi.ScaleX(14);
        }
        CRect rt(x, b.rc.top, b.rc.right, b.rc.bottom);
        dc.SetBkMode(TRANSPARENT);
        CFont* old = dc.SelectObject(&m_fontBold);
        dc.SetTextColor(pal.text);
        dc.DrawText(title, rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        rt.left += dc.GetTextExtent(title).cx + dpi.ScaleX(10);
        dc.SelectObject(&m_font);
        if (rt.left < rt.right) {
            dc.SetTextColor(pal.textFade);
            dc.DrawText(detail, rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        dc.SelectObject(old);
        return;
    }

    // Set start / set end: LosslessCut's cut point buttons, tinted with the segment color.
    if (b.id == Btn::SetStart || b.id == Btn::SetEnd) {
        const auto& llc = m_pMainFrame->m_llc;
        const int cur = llc.GetCurrentIndex();
        const int colorIndex = cur >= 0 ? cur : static_cast<int>(llc.GetSegments().size());
        COLORREF fill = llcui::CutpointFill(colorIndex, pal);
        COLORREF border = llcui::CutpointBorder(colorIndex, pal);
        if (!b.enabled) {
            fill = llcui::Blend(fill, pal.bg, 0.35);
            border = llcui::Blend(border, pal.bg, 0.35);
        } else if (pressed) {
            fill = llcui::Blend(RGB(0, 0, 0), fill, 0.15);
        } else if (hot) {
            fill = llcui::Blend(RGB(255, 255, 255), fill, 0.12);
        }
        GraphicsPath path;
        llcui::AddRoundRect(path, rf, radius);
        SolidBrush fb(llcui::ToGdi(fill));
        g.FillPath(&fb, &path);
        // the thick outer edge (left for start, right for end)
        const float bw = static_cast<float>(dpi.ScaleX(3));
        Region clip(&path);
        g.SetClip(&clip);
        SolidBrush bb(llcui::ToGdi(border));
        if (b.id == Btn::SetStart) {
            g.FillRectangle(&bb, rf.X, rf.Y, bw, rf.Height);
        } else {
            g.FillRectangle(&bb, rf.GetRight() - bw, rf.Y, bw, rf.Height);
        }
        g.ResetClip();
        iconBox.X += (b.id == Btn::SetStart ? bw : -bw) / 2;
        DrawIcon(g, b.id, iconBox, Color(b.enabled ? 255 : 120, 255, 255, 255), pal);
        return;
    }

    const bool isScope = b.id == Btn::ScopeFull || b.id == Btn::ScopeSegments || b.id == Btn::ScopeSingle;
    const bool isToggle = isScope || b.id == Btn::Restrict || b.id == Btn::Loop;
    const bool isKeyframe = b.id == Btn::PrevKeyframe || b.id == Btn::NextKeyframe;

    COLORREF bg = pal.bg;
    bool fillBg = false;
    if (b.active && isToggle) {
        bg = pressed ? llcui::Blend(RGB(0, 0, 0), pal.accent, 0.15) : hot ? llcui::Blend(RGB(255, 255, 255), pal.accent, 0.12) : pal.accent;
        fillBg = true;
    } else if (pressed) {
        bg = pal.pressed;
        fillBg = true;
    } else if (hot) {
        bg = pal.hover;
        fillBg = true;
    }
    if (b.active && isKeyframe && !fillBg) {
        bg = llcui::Blend(pal.keyframe, pal.bg, pal.dark ? 0.16 : 0.14);
        fillBg = true;
    } else if (b.active && isKeyframe) {
        bg = llcui::Blend(pal.keyframe, bg, 0.2);
    }
    if (fillBg) {
        GraphicsPath path;
        RectF r = rf;
        if (isScope) {
            r.Inflate(-static_cast<REAL>(dpi.ScaleX(2)), -static_cast<REAL>(dpi.ScaleY(2)));
        }
        llcui::AddRoundRect(path, r, isScope ? radius - 2 : radius);
        SolidBrush fb(llcui::ToGdi(bg));
        g.FillPath(&fb, &path);
    }

    if (isKeyframe) {
        // the key needs a little more room than the other glyphs to read clearly
        const REAL grow = iconBox.Width * 0.2f;
        iconBox.Inflate(grow / 2, grow / 2);
    }

    COLORREF fg = b.enabled ? pal.text : pal.textDisabled;
    if (b.active && isToggle) {
        fg = RGB(255, 255, 255);
    } else if (b.active && isKeyframe) {
        fg = pal.keyframe;
    } else if (b.enabled && b.id != Btn::Launch && !hot) {
        fg = llcui::Blend(pal.text, pal.bg, 0.86);
    }

    if (isScope) {
        const int i = b.id == Btn::ScopeFull ? 0 : b.id == Btn::ScopeSegments ? 1 : 2;
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(fg);
        CFont* old = dc.SelectObject(b.active ? &m_fontBold : &m_font);
        CRect rt(b.rc);
        dc.DrawText(kScopeLabels[i], rt, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        dc.SelectObject(old);
        return;
    }

    if (b.id == Btn::Launch && m_launchBleed > 0) {
        iconBox.X = static_cast<REAL>(b.rc.left); // content starts at the button's own edge
        DrawIcon(g, b.id, iconBox, llcui::ToGdi(fg), pal);
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(fg);
        CFont* old = dc.SelectObject(&m_fontBold);
        CRect rt(b.rc);
        rt.left = static_cast<int>(iconBox.GetRight()) + dpi.ScaleX(6);
        dc.DrawText(kLaunchLabel, rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        dc.SelectObject(old);
        return;
    }

    DrawIcon(g, b.id, iconBox, llcui::ToGdi(fg), pal);
}

void CLosslessCutStrip::OnPaint()
{
    CPaintDC paintDC(this);
    CRect client;
    GetClientRect(client);
    if (client.IsRectEmpty()) {
        return;
    }
    EnsureFonts();
    UpdateStates();
    const llcui::Palette pal = llcui::GetPalette();

    CDC dc;
    dc.CreateCompatibleDC(&paintDC);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&paintDC, client.Width(), client.Height());
    CBitmap* oldBmp = dc.SelectObject(&bmp);
    dc.FillSolidRect(client, pal.bg);

    {
        Graphics g(dc.GetSafeHdc());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        // separators
        SolidBrush sep(llcui::ToGdi(pal.separator));
        for (const CRect& r : m_separators) {
            g.FillRectangle(&sep, r.left, r.top, r.Width(), r.Height());
        }

        // segmented control frame behind the scope buttons
        const Button* a = nullptr;
        const Button* z = nullptr;
        for (const auto& b : m_buttons) {
            if (b.visible && (b.id == Btn::ScopeFull || b.id == Btn::ScopeSegments || b.id == Btn::ScopeSingle)) {
                if (!a) {
                    a = &b;
                }
                z = &b;
            }
        }
        if (a && z) {
            const RectF r(static_cast<REAL>(a->rc.left), static_cast<REAL>(a->rc.top), static_cast<REAL>(z->rc.right - a->rc.left), static_cast<REAL>(a->rc.Height()));
            GraphicsPath path;
            llcui::AddRoundRect(path, r, static_cast<float>(m_pMainFrame->m_dpi.ScaleX(6)));
            SolidBrush fill(llcui::ToGdi(llcui::Blend(pal.text, pal.bg, pal.dark ? 0.05 : 0.04)));
            g.FillPath(&fill, &path);
            Pen frame(llcui::ToGdi(pal.separator), 1.0f);
            g.DrawPath(&frame, &path);
        }

        for (int i = 0; i < static_cast<int>(m_buttons.size()); i++) {
            if (m_buttons[i].visible) {
                DrawButton(g, dc, m_buttons[i], i, pal);
            }
        }
    }

    paintDC.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
    dc.SelectObject(oldBmp);
}

BOOL CLosslessCutStrip::OnEraseBkgnd(CDC* /*pDC*/)
{
    return TRUE;
}

// -------------------------------------------------------------------------------------------------
// Input

BEGIN_MESSAGE_MAP(CLosslessCutStrip, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_RBUTTONUP()
    ON_WM_CAPTURECHANGED()
    ON_WM_SETCURSOR()
    ON_WM_MOUSEACTIVATE()
    ON_NOTIFY_EX_RANGE(TTN_NEEDTEXT, 0, 0xFFFF, OnToolTipText)
END_MESSAGE_MAP()

void CLosslessCutStrip::OnSize(UINT nType, int cx, int cy)
{
    __super::OnSize(nType, cx, cy);
    Layout();
    Invalidate(FALSE);
}

void CLosslessCutStrip::OnMouseMove(UINT nFlags, CPoint point)
{
    __super::OnMouseMove(nFlags, point);
    if (!m_bTracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
        m_bTracking = !!TrackMouseEvent(&tme);
    }
    UpdateStates();
    int hot = HitTest(point);
    if (hot >= 0 && !m_buttons[hot].enabled && m_buttons[hot].id != Btn::Status) {
        hot = -1;
    }
    if (hot != m_hot) {
        m_hot = hot;
        Invalidate(FALSE);
    }
}

void CLosslessCutStrip::OnMouseLeave()
{
    m_bTracking = false;
    if (m_hot != -1) {
        m_hot = -1;
        Invalidate(FALSE);
    }
}

void CLosslessCutStrip::OnLButtonDown(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    UpdateStates();
    const int idx = HitTest(point);
    if (idx >= 0 && (m_buttons[idx].enabled || m_buttons[idx].id == Btn::Status)) {
        m_pressed = idx;
        m_hot = idx;
        SetCapture();
        Invalidate(FALSE);
    }
}

void CLosslessCutStrip::OnLButtonUp(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    const int pressed = m_pressed;
    m_pressed = -1;
    if (GetCapture() == this) {
        ReleaseCapture();
    }
    if (pressed >= 0 && HitTest(point) == pressed) {
        const Button b = m_buttons[pressed];
        Execute(b);
    }
    Invalidate(FALSE);
}

void CLosslessCutStrip::OnLButtonDblClk(UINT nFlags, CPoint point)
{
    OnLButtonDown(nFlags, point);
}

void CLosslessCutStrip::OnRButtonUp(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    ClientToScreen(&point);
    m_pMainFrame->m_llc.ShowContextMenu(this, point, m_pMainFrame->GetPos());
}

void CLosslessCutStrip::OnCaptureChanged(CWnd* pWnd)
{
    __super::OnCaptureChanged(pWnd);
    if (pWnd != this && m_pressed >= 0) {
        m_pressed = -1;
        Invalidate(FALSE);
    }
}

BOOL CLosslessCutStrip::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
    UNREFERENCED_PARAMETER(pWnd);
    UNREFERENCED_PARAMETER(nHitTest);
    UNREFERENCED_PARAMETER(message);
    return TRUE;
}

int CLosslessCutStrip::OnMouseActivate(CWnd* pDesktopWnd, UINT nHitTest, UINT message)
{
    return __super::OnMouseActivate(pDesktopWnd, nHitTest, message);
}

BOOL CLosslessCutStrip::OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult)
{
    UNREFERENCED_PARAMETER(id);
    auto* pTTT = reinterpret_cast<NMTTDISPINFO*>(pNMHDR);
    const int idx = static_cast<int>(pNMHDR->idFrom) - 1;
    if (pNMHDR->hwndFrom != m_tooltip.m_hWnd || idx < 0 || idx >= static_cast<int>(m_buttons.size())) {
        return FALSE;
    }
    UpdateStates();
    m_tipText = TooltipFor(m_buttons[idx]);
    pTTT->lpszText = const_cast<LPTSTR>(m_tipText.GetString());
    pTTT->hinst = nullptr;
    *pResult = 0;
    return TRUE;
}
