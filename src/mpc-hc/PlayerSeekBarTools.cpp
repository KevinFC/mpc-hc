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

// The seekbar's zoom (with the overview that shows which part of the timeline is in view),
// smooth drag seeking, and the two buttons left of the channel that toggle them.

#include "stdafx.h"
#include "PlayerSeekBar.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "CMPCTheme.h"
#include "PPageAccelTbl.h"
#include <cmath>

using namespace Gdiplus;

namespace
{
    constexpr double kWheelZoomStep = 1.5;      // per wheel notch
    constexpr double kCommandZoomStep = 2.0;    // zoom in / zoom out commands
    constexpr double kButtonZoom = 4.0;         // the zoom button
    constexpr double kMaxZoom = 5000.0;
    constexpr double kMinViewSpan = 5000000.0;  // 0.5 s
    constexpr double kViewTauMs = 65.0;         // zoom and scroll animation
    constexpr int kOverviewHeight = 12;         // dialog px
    constexpr int kOverviewTrackTop = 3;
    constexpr int kOverviewTrackHeight = 7;
    constexpr int kOverviewLabelWidth = 30;     // the zoom factor, left of the overview track

    enum : UINT_PTR { TIP_LABEL = 1, TIP_OVERVIEW };

    double NowMs()
    {
        static const double freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart * 1000.0 / freq;
    }

    // How long the glide takes to cover about two thirds of the distance: 80 ms (1) to 2.3 s (10).
    // Light (3) is about 170 ms, medium (5) 350 ms, strong (7) 750 ms, very strong (9) 1.6 s.
    double GlideTauMs(int strength)
    {
        return 80.0 * std::pow(1.45, std::clamp(strength, 1, 10) - 1);
    }

    CString FormatZoom(double zoom)
    {
        CString s;
        if (zoom < 9.95 && std::abs(zoom - std::round(zoom)) >= 0.05) {
            s.Format(_T("%.1f\x00D7"), zoom);
        } else {
            s.Format(_T("%.0f\x00D7"), zoom);
        }
        return s;
    }

    CString FormatTime(double rt)
    {
        const LONGLONG total = static_cast<LONGLONG>(std::max(0.0, rt) / 10000000.0);
        CString s;
        if (total >= 3600) {
            s.Format(_T("%I64d:%02I64d:%02I64d"), total / 3600, total / 60 % 60, total % 60);
        } else {
            s.Format(_T("%I64d:%02I64d"), total / 60, total % 60);
        }
        return s;
    }

    CString WithShortcut(CString text, UINT cmd)
    {
        const CString keys = CPPageAccelTbl::MakeAccelShortcutLabel(cmd);
        if (!keys.IsEmpty()) {
            text += _T("  (") + keys + _T(")");
        }
        return text;
    }
}

// -------------------------------------------------------------------------------------------------
// The view

double CPlayerSeekBar::ViewSpan() const
{
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    if (!m_bHasDuration || full <= 0.0) {
        return 1.0;
    }
    return (m_viewSpan > 0.0 && m_viewSpan < full) ? m_viewSpan : full;
}

REFERENCE_TIME CPlayerSeekBar::ViewStart() const
{
    if (m_bHasDuration && m_viewSpan > 0.0 && m_viewSpan < static_cast<double>(m_rtStop - m_rtStart)) {
        return std::llround(m_viewStart);
    }
    return m_rtStart;
}

REFERENCE_TIME CPlayerSeekBar::ViewStop() const
{
    return m_bHasDuration ? std::min(m_rtStop, ViewStart() + std::llround(ViewSpan())) : m_rtStop;
}

bool CPlayerSeekBar::InView(REFERENCE_TIME rt) const
{
    return rt >= ViewStart() && rt <= ViewStop();
}

bool CPlayerSeekBar::IsZoomed() const
{
    return m_bHasDuration && m_viewTargetSpan > 0.0 && m_viewTargetSpan < static_cast<double>(m_rtStop - m_rtStart);
}

double CPlayerSeekBar::ZoomFactor() const
{
    return IsZoomed() ? static_cast<double>(m_rtStop - m_rtStart) / m_viewTargetSpan : 1.0;
}

double CPlayerSeekBar::MinViewSpan() const
{
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    return std::min(full, std::max(kMinViewSpan, full / kMaxZoom));
}

void CPlayerSeekBar::SetViewTarget(double start, double span, bool bAnimate)
{
    if (!m_bHasDuration) {
        return;
    }
    const bool bWasZoomed = IsZoomed();
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    span = std::clamp(span, MinViewSpan(), full);
    if (span >= full * 0.999) {
        span = full;
    }
    start = std::clamp(start, static_cast<double>(m_rtStart), static_cast<double>(m_rtStop) - span);
    m_viewTargetStart = start;
    m_viewTargetSpan = span;
    if (m_viewSpan <= 0.0 || m_viewSpan >= full) {
        m_viewStart = static_cast<double>(m_rtStart);
        m_viewSpan = full;
    }
    if (bAnimate) {
        StartAnimation();
    } else {
        m_viewStart = start;
        m_viewSpan = span;
        m_bViewAnchor = false;
        if (!IsZoomed()) {
            m_viewSpan = 0.0; // back to the plain full-range mapping
        }
    }
    Invalidate();
    OnZoomStateMaybeChanged(bWasZoomed);
}

void CPlayerSeekBar::ZoomAround(double factor, double anchorTime, double anchorFrac)
{
    if (!m_bHasDuration || factor <= 0.0) {
        return;
    }
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    const double span = std::clamp((IsZoomed() ? m_viewTargetSpan : full) / factor, MinViewSpan(), full);
    m_anchorTime = anchorTime;
    m_anchorFrac = std::clamp(anchorFrac, 0.0, 1.0);
    SetViewTarget(anchorTime - m_anchorFrac * span, span, true);
    m_bViewAnchor = true;
}

void CPlayerSeekBar::ZoomBy(double factor)
{
    if (!m_bHasDuration) {
        return;
    }
    // keep the playhead where it is on screen; zoom around the middle when it is out of view
    double anchor = static_cast<double>(m_rtPosDraw);
    if (!InView(m_rtPosDraw)) {
        anchor = ViewStart() + ViewSpan() / 2.0;
    }
    ZoomAround(factor, anchor, (anchor - ViewStart()) / ViewSpan());
}

void CPlayerSeekBar::ResetZoom()
{
    if (IsZoomed()) {
        ZoomBy(1.0 / ZoomFactor());
    }
}

void CPlayerSeekBar::PanTo(double start)
{
    if (IsZoomed()) {
        SetViewTarget(start, m_viewTargetSpan, false);
    }
}

void CPlayerSeekBar::FollowPlayhead(REFERENCE_TIME rtOld, REFERENCE_TIME rtNew)
{
    if (!IsZoomed() || m_bPanning || rtNew == rtOld) {
        return;
    }
    const double span = m_viewTargetSpan;
    const double start = m_viewTargetStart, stop = start + span;
    if (rtNew >= start && rtNew <= stop) {
        return;
    }
    const bool bWasInView = rtOld >= start && rtOld <= stop;
    if (!bWasInView && GetTickCount64() - m_lastUserPanTick < 2000) {
        return; // the user just moved the view somewhere else
    }
    m_bViewAnchor = false;
    if (rtNew > stop && rtNew - stop < span / 4) {
        SetViewTarget(rtNew - span * 0.05, span, true); // playing past the right edge: turn the page
    } else {
        SetViewTarget(rtNew - span / 2, span, true);
    }
}

void CPlayerSeekBar::OnZoomStateMaybeChanged(bool bWasZoomed)
{
    if (IsZoomed() == bWasZoomed || !m_hWnd) {
        return;
    }
    // the overview appears or goes away: the bar changes height
    const int h = m_pMainFrame->m_dpi.ScaleY(kOverviewHeight);
    const int now = CalcFixedLayout(TRUE, TRUE).cy;
    m_lastOverviewX = INT_MIN;
    HideToolTip();
    RelayoutAfterHeightChange(bWasZoomed ? now + h : now - h);
    Invalidate();
    RefreshToolButtons();
}

void CPlayerSeekBar::RelayoutAfterHeightChange(int heightBefore)
{
    CMainFrame& frame = *m_pMainFrame;
    const int after = CalcFixedLayout(TRUE, TRUE).cy;
    if (after != heightBefore && IsWindowVisible() && !frame.IsZoomed() && !frame.IsIconic() &&
            !frame.IsFullScreenMode() && !frame.m_controls.ToolbarsCoverVideo()) {
        // grow/shrink the window like toggling a toolbar does, so the video keeps its size
        CRect r;
        frame.GetWindowRect(r);
        r.bottom += after - heightBefore;
        frame.MoveWindow(r);
    }
    frame.RecalcLayout();
}

// -------------------------------------------------------------------------------------------------
// Animation: the view zooming/scrolling, the thumb gliding, and scrolling while dragging past an edge

void CPlayerSeekBar::StartAnimation()
{
    if (m_hWnd && !m_bAnimating) {
        m_bAnimating = true;
        m_lastAnimMs = NowMs();
        SetTimer(TIMER_ANIMATE, 15, nullptr);
    }
}

void CPlayerSeekBar::OnAnimate()
{
    const double now = NowMs();
    const double dt = std::clamp(now - m_lastAnimMs, 1.0, 100.0);
    m_lastAnimMs = now;
    bool bKeep = false;

    // dragging the thumb past either end of a zoomed-in channel scrolls the view that way
    if (m_bDraggingThumb && IsZoomed()) {
        bKeep = true;
        CPoint pt;
        GetCursorPos(&pt);
        ScreenToClient(&pt);
        const CRect ch(GetChannelRect());
        const int over = pt.x < ch.left ? pt.x - ch.left : pt.x > ch.right ? pt.x - ch.right : 0;
        if (over != 0) {
            const double spansPerSecond = std::clamp(over / static_cast<double>(m_pMainFrame->m_dpi.ScaleX(80)), -3.0, 3.0);
            PanTo(m_viewTargetStart + m_viewTargetSpan * spansPerSecond * dt / 1000.0);
            if (IsSmoothDrag()) {
                SetGlideTarget(pt);
            } else {
                MoveThumb(pt);
                SeekToThumb(pt, SeekMode::Drag);
            }
        }
    }

    // zoom/scroll toward the target view: the span eases on a log scale, so zooming feels even
    if (m_viewSpan > 0.0 && m_viewTargetSpan > 0.0) {
        const double px = m_viewTargetSpan / std::max(1, GetChannelRect().Width());
        const bool bSpanDone = std::abs(m_viewSpan / m_viewTargetSpan - 1.0) < 0.002;
        const bool bStartDone = std::abs(m_viewStart - m_viewTargetStart) < px * 0.5;
        if (bSpanDone && bStartDone) {
            if (m_viewSpan != m_viewTargetSpan || m_viewStart != m_viewTargetStart) {
                m_viewSpan = m_viewTargetSpan;
                m_viewStart = m_viewTargetStart;
                m_bViewAnchor = false;
                Invalidate();
            }
            if (!IsZoomed()) {
                m_viewSpan = 0.0;
            }
        } else {
            const double k = 1.0 - std::exp(-dt / kViewTauMs);
            const double span = std::exp(std::log(m_viewSpan) + (std::log(m_viewTargetSpan) - std::log(m_viewSpan)) * k);
            double start = m_bViewAnchor ? m_anchorTime - m_anchorFrac * span : m_viewStart + (m_viewTargetStart - m_viewStart) * k;
            start = std::clamp(start, static_cast<double>(m_rtStart), static_cast<double>(m_rtStop) - span);
            m_viewSpan = span;
            m_viewStart = start;
            Invalidate();
            bKeep = true;
        }
    }

    // smooth drag: the thumb eases toward the mouse and seeks along the way
    if (m_bGliding) {
        const double k = 1.0 - std::exp(-dt / GlideTauMs(AfxGetAppSettings().iSeekbarSmoothStrength));
        m_glidePos += (m_glideTarget - m_glidePos) * k;
        const double px = ViewSpan() / std::max(1, GetChannelRect().Width());
        const bool bArrived = std::abs(m_glideTarget - m_glidePos) <= std::max(px * 0.5, 10000.0);
        if (bArrived) {
            m_glidePos = static_cast<double>(m_glideTarget);
        }
        const REFERENCE_TIME rtOldPos = m_rtPos;
        MoveThumbToTime(std::llround(m_glidePos));
        if (bArrived) {
            m_bGliding = false; // caught up with the mouse; the next move glides again
            if (m_rtPos != rtOldPos) {
                SeekToThumb(m_glidePoint, SeekMode::Drag);
            }
        } else {
            if (m_rtPos != rtOldPos) {
                SeekToThumb(m_glidePoint, SeekMode::Drag);
            }
            bKeep = true;
        }
    }

    if (!bKeep) {
        KillTimer(TIMER_ANIMATE);
        m_bAnimating = false;
    }
}

// -------------------------------------------------------------------------------------------------
// Smooth and exact drag seeking

bool CPlayerSeekBar::IsSmoothDrag() const
{
    return AfxGetAppSettings().bSeekbarSmoothDrag;
}

bool CPlayerSeekBar::IsExactDrag() const
{
    return AfxGetAppSettings().bSeekbarExactDrag;
}

void CPlayerSeekBar::SetGlideTarget(CPoint point)
{
    if (!m_bHasDuration) {
        return;
    }
    m_glidePoint = point;
    m_glideTarget = m_pMainFrame->m_llc.ClampToRanges(PositionFromClientPoint(point));
    m_bGliding = true;
    StartAnimation();
}

void CPlayerSeekBar::FinishDrag()
{
    pauseAfterFirstScroll = false;
    if (pausedForScrolling) {
        pausedForScrolling = false;
        m_pMainFrame->MediaControlRun();
    }
}

void CPlayerSeekBar::SetSmoothStrength(int strength)
{
    auto& s = AfxGetAppSettings();
    strength = std::clamp(strength, 1, 10);
    if (strength != s.iSeekbarSmoothStrength) {
        s.iSeekbarSmoothStrength = strength;
        CString msg;
        msg.Format(_T("Smooth drag seeking: smoothing %d of 10"), strength);
        m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, msg, 1500);
        RefreshToolButtons();
    }
}

void CPlayerSeekBar::RefreshToolButtons()
{
    if (::IsWindow(m_pMainFrame->m_wndToolBar.m_seekTools.m_hWnd)) {
        m_pMainFrame->m_wndToolBar.m_seekTools.SyncState();
    }
}

// -------------------------------------------------------------------------------------------------
// The overview: a label with the zoom factor, then the whole timeline with the view as a window on it

int CPlayerSeekBar::OverviewHeight() const
{
    return IsZoomed() ? m_pMainFrame->m_dpi.ScaleY(kOverviewHeight) : 0;
}

CRect CPlayerSeekBar::GetOverviewLabelRect() const
{
    if (!OverviewHeight()) {
        return CRect();
    }
    CRect client;
    GetClientRect(&client);
    const CRect ch(GetChannelRect());
    return CRect(ch.left, client.top, ch.left + m_pMainFrame->m_dpi.ScaleX(kOverviewLabelWidth), client.top + OverviewHeight() + 1);
}

CRect CPlayerSeekBar::GetOverviewRect() const
{
    if (!OverviewHeight()) {
        return CRect();
    }
    CRect client;
    GetClientRect(&client);
    const CRect ch(GetChannelRect());
    const int top = client.top + m_pMainFrame->m_dpi.ScaleY(kOverviewTrackTop);
    const int left = GetOverviewLabelRect().right + m_pMainFrame->m_dpi.ScaleX(4);
    return CRect(left, top, std::max(left + 1, static_cast<int>(ch.right)), top + m_pMainFrame->m_dpi.ScaleY(kOverviewTrackHeight));
}

double CPlayerSeekBar::OverviewTimeAt(int x) const
{
    const CRect track(GetOverviewRect());
    return m_rtStart + (x - track.left) * static_cast<double>(m_rtStop - m_rtStart) / std::max(1, track.Width());
}

CRect CPlayerSeekBar::GetOverviewWindowRect() const
{
    const CRect track(GetOverviewRect());
    if (track.IsRectEmpty()) {
        return CRect();
    }
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    auto X = [&](double t) {
        return track.left + static_cast<int>(std::lround((t - m_rtStart) * track.Width() / full));
    };
    int x0 = X(static_cast<double>(ViewStart()));
    int x1 = X(static_cast<double>(ViewStop()));
    const int minW = m_pMainFrame->m_dpi.ScaleX(8);
    if (x1 - x0 < minW) {
        const int c = (x0 + x1) / 2;
        x0 = std::clamp<int>(c - minW / 2, track.left, track.right - minW);
        x1 = x0 + minW;
    }
    const int grow = m_pMainFrame->m_dpi.ScaleY(2);
    return CRect(x0, track.top - grow, x1, track.bottom + grow);
}

CPlayerSeekBar::OverviewHit CPlayerSeekBar::OverviewHitTest(CPoint point) const
{
    if (!OverviewHeight()) {
        return OverviewHit::None;
    }
    if (GetOverviewLabelRect().PtInRect(point)) {
        return OverviewHit::Label;
    }
    CRect client;
    GetClientRect(&client);
    CRect band(GetOverviewRect());
    band.top = client.top;
    band.bottom = client.top + OverviewHeight() + 1;
    band.InflateRect(m_pMainFrame->m_dpi.ScaleX(4), 0);
    if (!band.PtInRect(point)) {
        return OverviewHit::None;
    }
    // the window's ends stretch the view; between them it moves (like a DAW's scrollbar)
    const CRect win(GetOverviewWindowRect());
    const int grip = std::min(m_pMainFrame->m_dpi.ScaleX(5), win.Width() / 3);
    if (std::abs(point.x - win.left) <= grip) {
        return OverviewHit::LeftEdge;
    }
    if (std::abs(point.x - win.right) <= grip) {
        return OverviewHit::RightEdge;
    }
    return (point.x > win.left && point.x < win.right) ? OverviewHit::Window : OverviewHit::Track;
}

void CPlayerSeekBar::DragOverview(int x)
{
    if (!IsZoomed()) {
        return;
    }
    const double t = OverviewTimeAt(x);
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    const double maxSpan = full * 0.995; // stays zoomed in while dragging; letting go near full shows everything
    const double start = m_viewTargetStart, stop = m_viewTargetStart + m_viewTargetSpan;
    switch (m_panPart) {
        case OverviewHit::LeftEdge: {
            const double s = std::clamp(t, static_cast<double>(m_rtStart), stop - MinViewSpan());
            const double span = std::min(stop - s, maxSpan);
            SetViewTarget(stop - span, span, false);
            break;
        }
        case OverviewHit::RightEdge: {
            const double e = std::clamp(t, start + MinViewSpan(), static_cast<double>(m_rtStop));
            SetViewTarget(start, std::min(e - start, maxSpan), false);
            break;
        }
        default:
            PanTo(t - m_panGrab);
            break;
    }
    m_lastUserPanTick = GetTickCount64();
}

void CPlayerSeekBar::EndOverviewDrag()
{
    m_bPanning = false;
    m_panPart = OverviewHit::None;
    if (GetCapture() == this) {
        ReleaseCapture();
    }
    if (IsZoomed() && m_viewTargetSpan >= static_cast<double>(m_rtStop - m_rtStart) * 0.98) {
        ResetZoom(); // stretched out to (nearly) everything
    }
    Invalidate();
}

void CPlayerSeekBar::DrawOverview(CDC& dc)
{
    if (!OverviewHeight() || !m_bHasDuration) {
        return;
    }
    const auto& dpi = m_pMainFrame->m_dpi;
    const llcui::Palette pal = llcui::GetPalette();
    const CRect track(GetOverviewRect());
    const CRect win(GetOverviewWindowRect());
    const double full = static_cast<double>(m_rtStop - m_rtStart);
    auto X = [&](double t) {
        return static_cast<REAL>(track.left + std::clamp((t - m_rtStart) / full, 0.0, 1.0) * track.Width());
    };

    {
        Graphics g(dc.GetSafeHdc());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        const RectF tr(static_cast<REAL>(track.left), static_cast<REAL>(track.top), static_cast<REAL>(track.Width()), static_cast<REAL>(track.Height()));
        GraphicsPath trackPath;
        llcui::AddRoundRect(trackPath, tr, tr.Height / 2);
        const COLORREF trackColor = llcui::Blend(pal.text, pal.bg, pal.dark ? 0.11 : 0.10);
        SolidBrush trackBrush(llcui::ToGdi(trackColor));
        g.FillPath(&trackBrush, &trackPath);

        Region clip(&trackPath);
        g.SetClip(&clip);

        // what has been played, or the LosslessCut segments
        const auto& llc = m_pMainFrame->m_llc;
        if (m_bLlcMode && llc.IsActive()) {
            const auto& segs = llc.GetSegments();
            const int current = llc.GetCurrentIndex();
            for (int i = 0; i < static_cast<int>(segs.size()); i++) {
                const auto& seg = segs[i];
                if (seg.IsMarker()) {
                    continue;
                }
                const REAL x0 = X(static_cast<double>(CLosslessCutController::ToRT(seg.start)));
                const REAL x1 = std::max(x0 + 1.0f, X(static_cast<double>(CLosslessCutController::ToRT(*seg.end))));
                const llcui::SegState state = i == current ? llcui::SegState::Current : seg.selected ? llcui::SegState::Normal : llcui::SegState::Excluded;
                SolidBrush b(llcui::ToGdi(llcui::SegmentFill(i, state, pal)));
                g.FillRectangle(&b, x0, tr.Y, x1 - x0, tr.Height);
            }
        } else {
            SolidBrush played(llcui::ToGdi(llcui::Blend(pal.accent, trackColor, 0.55)));
            g.FillRectangle(&played, tr.X, tr.Y, X(static_cast<double>(m_rtPosDraw)) - tr.X, tr.Height);
        }

        // chapters
        {
            CAutoLock lock(&m_csChapterBag);
            if (m_pChapterBag) {
                SolidBrush tick(llcui::ToGdi(llcui::Blend(pal.text, trackColor, 0.45)));
                for (DWORD i = 0; i < m_pChapterBag->ChapGetCount(); i++) {
                    REFERENCE_TIME rtChap;
                    if (SUCCEEDED(m_pChapterBag->ChapGet(i, &rtChap, nullptr))) {
                        g.FillRectangle(&tick, X(static_cast<double>(rtChap)), tr.Y, 1.0f, tr.Height);
                    }
                }
            }
        }

        // dim what is outside the view
        SolidBrush dim(llcui::ToGdi(pal.bg, 120));
        if (win.left > track.left) {
            g.FillRectangle(&dim, tr.X, tr.Y, static_cast<REAL>(win.left) - tr.X, tr.Height);
        }
        if (win.right < track.right) {
            g.FillRectangle(&dim, static_cast<REAL>(win.right), tr.Y, tr.GetRight() - win.right, tr.Height);
        }
        g.ResetClip();

        // the view: a lens over the track
        const OverviewHit hot = m_bPanning ? m_panPart : m_overviewHot;
        const RectF wr(static_cast<REAL>(win.left), static_cast<REAL>(win.top), static_cast<REAL>(win.Width()), static_cast<REAL>(win.Height()));
        GraphicsPath winPath;
        llcui::AddRoundRect(winPath, wr, static_cast<REAL>(dpi.ScaleX(3)));
        const bool bHot = hot == OverviewHit::Window || hot == OverviewHit::LeftEdge || hot == OverviewHit::RightEdge;
        SolidBrush lens(llcui::ToGdi(pal.accent, m_bPanning && hot == OverviewHit::Window ? 110 : bHot ? 80 : 50));
        g.FillPath(&lens, &winPath);
        Pen frame(llcui::ToGdi(pal.accent, bHot ? 255 : 215), static_cast<REAL>(std::max(1.0, dpi.ScaleX(3) / 2.0)));
        g.DrawPath(&frame, &winPath);

        // the ends can be dragged: show a grip on the one under the mouse
        if (hot == OverviewHit::LeftEdge || hot == OverviewHit::RightEdge) {
            const REAL gw = static_cast<REAL>(std::max(2, dpi.ScaleX(3)));
            const REAL gx = hot == OverviewHit::LeftEdge ? wr.X : wr.GetRight() - gw;
            GraphicsPath gripPath;
            llcui::AddRoundRect(gripPath, RectF(gx, wr.Y, gw, wr.Height), gw / 2);
            SolidBrush grip(llcui::ToGdi(llcui::Blend(RGB(255, 255, 255), pal.accent, 0.35)));
            g.FillPath(&grip, &gripPath);
        }

        // the playhead
        SolidBrush head(llcui::ToGdi(pal.dark ? RGB(240, 240, 240) : RGB(20, 20, 20)));
        const REAL hx = X(static_cast<double>(m_rtPosDraw));
        g.FillRectangle(&head, hx - 1.0f, wr.Y + 1.0f, 2.0f, wr.Height - 2.0f);
    }

    // the zoom factor, left of the track
    const CRect label(GetOverviewLabelRect());
    const int fontH = dpi.ScaleY(10);
    if (!m_overviewFont.GetSafeHandle() || m_overviewFontHeight != fontH) {
        m_overviewFont.DeleteObject();
        LOGFONT lf = {};
        CMPCThemeUtil::getFontByType(m_overviewFont, this, CMPCThemeUtil::MessageFont, false, true);
        m_overviewFont.GetLogFont(&lf);
        m_overviewFont.DeleteObject();
        lf.lfHeight = -fontH;
        lf.lfWeight = FW_BOLD;
        m_overviewFont.CreateFontIndirect(&lf);
        m_overviewFontHeight = fontH;
    }
    CFont* oldFont = dc.SelectObject(&m_overviewFont);
    const int oldMode = dc.SetBkMode(TRANSPARENT);
    const COLORREF oldColor = dc.SetTextColor(m_overviewHot == OverviewHit::Label ? pal.text : pal.textFade);
    CRect r(label);
    dc.DrawText(FormatZoom(ZoomFactor()), r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    dc.SetTextColor(oldColor);
    dc.SetBkMode(oldMode);
    dc.SelectObject(oldFont);
}

// -------------------------------------------------------------------------------------------------
// Tooltips and menus

void CPlayerSeekBar::UpdateToolTips()
{
    if (!m_toolTip.m_hWnd) {
        return;
    }
    // registered directly with TTF_SUBCLASS (MFC's AddTool(rect) leaves it out), and only moved
    // when they exist, so the hover delay doesn't restart on every repaint
    auto sync = [&](UINT_PTR id, const CRect& rc) {
        TOOLINFO ti = { sizeof(TOOLINFO) };
        ti.hwnd = m_hWnd;
        ti.uId = id;
        const bool exists = !!m_toolTip.SendMessage(TTM_GETTOOLINFO, 0, reinterpret_cast<LPARAM>(&ti));
        if (!rc.IsRectEmpty()) {
            ti.rect = rc;
            if (exists) {
                m_toolTip.SendMessage(TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
            } else {
                ti.uFlags = TTF_SUBCLASS;
                ti.lpszText = LPSTR_TEXTCALLBACK;
                m_toolTip.SendMessage(TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
            }
        } else if (exists) {
            m_toolTip.SendMessage(TTM_DELTOOL, 0, reinterpret_cast<LPARAM>(&ti));
        }
    };
    sync(TIP_LABEL, GetOverviewLabelRect());
    CRect overview(GetOverviewRect());
    if (!overview.IsRectEmpty()) {
        CRect client;
        GetClientRect(&client);
        overview.top = client.top;
        overview.bottom = client.top + OverviewHeight() + 1;
    }
    sync(TIP_OVERVIEW, overview);
}

CString CPlayerSeekBar::ToolTipFor(UINT_PTR id)
{
    CString text;
    switch (id) {
        case TIP_LABEL:
            text.Format(_T("Zoomed in %s. Click to show the whole timeline"), FormatZoom(ZoomFactor()).GetString());
            break;
        case TIP_OVERVIEW:
            text.Format(_T("Showing %s to %s of %s"), FormatTime(m_viewTargetStart - m_rtStart).GetString(),
                        FormatTime(m_viewTargetStart + m_viewTargetSpan - m_rtStart).GetString(), FormatTime(static_cast<double>(m_rtStop - m_rtStart)).GetString());
            text += _T("\nDrag the window to move the view, or its ends to widen or narrow it. Click to jump there, scroll to move it, double-click to show the whole timeline.");
            break;
    }
    return text;
}

BOOL CPlayerSeekBar::OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult)
{
    UNREFERENCED_PARAMETER(id);
    if (!m_toolTip.m_hWnd || pNMHDR->hwndFrom != m_toolTip.m_hWnd) {
        return FALSE;
    }
    auto* pTTT = reinterpret_cast<NMTTDISPINFO*>(pNMHDR);
    m_toolTipText = ToolTipFor(pNMHDR->idFrom);
    pTTT->lpszText = const_cast<LPTSTR>(m_toolTipText.GetString());
    pTTT->hinst = nullptr;
    *pResult = 0;
    return TRUE;
}

CString CPlayerSeekBar::SeekToolTip(SeekTool tool) const
{
    CString text;
    switch (tool) {
        case SeekTool::Smooth: {
            text = WithShortcut(IsSmoothDrag() ? _T("Smooth drag seeking is on") : _T("Smooth drag seeking is off"), ID_SEEKBAR_SMOOTH_DRAG);
            CString detail;
            detail.Format(_T("\nWhile you drag the seekbar, the playhead glides after the mouse and stops where it is when you let go. It seeks to exact frames, not keyframes.")
                          _T("\nSmoothing: %d of 10. Scroll the mouse wheel here to change it, or right-click for presets."),
                          AfxGetAppSettings().iSeekbarSmoothStrength);
            text += detail;
            break;
        }
        case SeekTool::Exact:
            text = WithShortcut(IsExactDrag() ? _T("Exact seeking is on") : _T("Exact seeking is off"), ID_SEEKBAR_EXACT_DRAG);
            text += IsExactDrag()
                    ? _T("\nDragging the seekbar and the hover preview show exact frames instead of the nearest keyframes (slower on long files).")
                    : _T("\nDragging the seekbar and the hover preview jump to the nearest keyframes, which is fast. Hold Shift for exact frames, or turn this on.");
            break;
        case SeekTool::Zoom:
            if (IsZoomed()) {
                text.Format(_T("Zoomed in %s. Click to show the whole timeline"), FormatZoom(ZoomFactor()).GetString());
                text = WithShortcut(text, ID_SEEKBAR_ZOOM_RESET);
            } else {
                text = WithShortcut(_T("Zoom in on the seekbar, around the playhead"), ID_SEEKBAR_ZOOM_IN);
            }
            text += _T("\nScroll the mouse wheel here, or hold Ctrl and scroll over the seekbar to zoom at the mouse. Right-click for more.");
            break;
    }
    return text;
}

void CPlayerSeekBar::OnSeekToolClicked(SeekTool tool)
{
    switch (tool) {
        case SeekTool::Smooth: OnSmoothDrag(); break;
        case SeekTool::Exact: OnExactDrag(); break;
        case SeekTool::Zoom:
            if (IsZoomed()) {
                ResetZoom();
            } else {
                ZoomBy(kButtonZoom);
            }
            break;
    }
}

void CPlayerSeekBar::ShowSeekToolMenu(SeekTool tool, CPoint screenPt, CWnd* pOwner)
{
    enum {
        CMD_SMOOTH = 1, CMD_EXACT, CMD_LEVEL_FIRST, CMD_LEVEL_LAST = CMD_LEVEL_FIRST + 3,
        CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, CMD_ZOOM_FIRST, CMD_ZOOM_LAST = CMD_ZOOM_FIRST + 4
    };
    const int levels[] = { 3, 5, 7, 9 };
    const LPCTSTR levelNames[] = { _T("Light"), _T("Medium"), _T("Strong"), _T("Very strong") };
    const double zooms[] = { 2, 4, 8, 16, 32 };
    const int strength = AfxGetAppSettings().iSeekbarSmoothStrength;

    HideToolTip();
    m_pMainFrame->PreviewWindowHide();
    m_toolMenu.DestroyMenu();
    m_toolMenu.CreatePopupMenu();
    switch (tool) {
        case SeekTool::Smooth: {
            m_toolMenu.AppendMenu(MF_STRING | (IsSmoothDrag() ? MF_CHECKED : MF_UNCHECKED), CMD_SMOOTH, _T("Smooth drag seeking"));
            m_toolMenu.AppendMenu(MF_SEPARATOR);
            CString current;
            current.Format(_T("Smoothing: %d of 10"), strength);
            m_toolMenu.AppendMenu(MF_STRING | MF_GRAYED, 0, current);
            for (int i = 0; i < _countof(levels); i++) {
                m_toolMenu.AppendMenu(MF_STRING | (strength == levels[i] ? MF_CHECKED : MF_UNCHECKED), CMD_LEVEL_FIRST + i, levelNames[i]);
            }
            break;
        }
        case SeekTool::Exact:
            m_toolMenu.AppendMenu(MF_STRING | (IsExactDrag() ? MF_CHECKED : MF_UNCHECKED), CMD_EXACT, _T("Exact seeking (ignore keyframes when dragging and in the hover preview)"));
            break;
        case SeekTool::Zoom: {
            const UINT bDur = m_bHasDuration ? MF_ENABLED : MF_GRAYED;
            m_toolMenu.AppendMenu(MF_STRING | bDur, CMD_ZOOM_IN, _T("Zoom in"));
            m_toolMenu.AppendMenu(MF_STRING | (IsZoomed() ? MF_ENABLED : MF_GRAYED), CMD_ZOOM_OUT, _T("Zoom out"));
            m_toolMenu.AppendMenu(MF_STRING | (IsZoomed() ? MF_ENABLED : MF_GRAYED), CMD_ZOOM_RESET, _T("Show the whole timeline"));
            m_toolMenu.AppendMenu(MF_SEPARATOR);
            for (int i = 0; i < _countof(zooms); i++) {
                const bool bChecked = IsZoomed() && std::abs(ZoomFactor() / zooms[i] - 1.0) < 0.01;
                m_toolMenu.AppendMenu(MF_STRING | bDur | (bChecked ? MF_CHECKED : MF_UNCHECKED), CMD_ZOOM_FIRST + i, FormatZoom(zooms[i]));
            }
            break;
        }
    }
    m_toolMenu.fulfillThemeReqs();
    const UINT cmd = m_toolMenu.TrackPopupMenu(TPM_LEFTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, pOwner ? pOwner : this);
    m_toolMenu.DestroyMenu();

    if (cmd == CMD_SMOOTH) {
        OnSmoothDrag();
    } else if (cmd == CMD_EXACT) {
        OnExactDrag();
    } else if (cmd >= CMD_LEVEL_FIRST && cmd <= CMD_LEVEL_LAST) {
        SetSmoothStrength(levels[cmd - CMD_LEVEL_FIRST]);
        if (!IsSmoothDrag()) {
            OnSmoothDrag(); // picking a level means wanting it on
        }
    } else if (cmd == CMD_ZOOM_IN) {
        OnZoomIn();
    } else if (cmd == CMD_ZOOM_OUT) {
        OnZoomOut();
    } else if (cmd == CMD_ZOOM_RESET) {
        OnZoomReset();
    } else if (cmd >= CMD_ZOOM_FIRST && cmd <= CMD_ZOOM_LAST) {
        ZoomBy(zooms[cmd - CMD_ZOOM_FIRST] / ZoomFactor());
    }
}

// -------------------------------------------------------------------------------------------------
// Input and commands

BOOL CPlayerSeekBar::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    CPoint point(pt);
    ScreenToClient(&point);
    const double notches = zDelta / static_cast<double>(WHEEL_DELTA);
    if (m_bHasDuration) {
        const OverviewHit hit = OverviewHitTest(point);
        if (hit != OverviewHit::None && !(nFlags & MK_CONTROL)) {
            // scrolling over the overview moves the view
            PanTo(m_viewTargetStart - notches * m_viewTargetSpan * 0.2);
            m_lastUserPanTick = GetTickCount64();
            return TRUE;
        }
        if ((nFlags & MK_CONTROL) && (hit != OverviewHit::None || GetSeekableRect().PtInRect(point))) {
            if (hit != OverviewHit::None) {
                ZoomAround(std::pow(kWheelZoomStep, notches), ViewStart() + ViewSpan() / 2.0, 0.5);
            } else {
                // zoom at the mouse: the time under it stays under it
                const CRect ch(GetChannelRect());
                const double frac = std::clamp((point.x - ch.left) / static_cast<double>(std::max(1, ch.Width())), 0.0, 1.0);
                ZoomAround(std::pow(kWheelZoomStep, notches), ViewStart() + frac * ViewSpan(), frac);
            }
            return TRUE;
        }
    }
    return __super::OnMouseWheel(nFlags, zDelta, pt);
}

void CPlayerSeekBar::OnRButtonUp(UINT nFlags, CPoint point)
{
    if (OverviewHitTest(point) != OverviewHit::None) {
        ClientToScreen(&point);
        ShowSeekToolMenu(SeekTool::Zoom, point, this);
        return;
    }
    __super::OnRButtonUp(nFlags, point);
}

void CPlayerSeekBar::OnSmoothDrag()
{
    auto& s = AfxGetAppSettings();
    s.bSeekbarSmoothDrag = !s.bSeekbarSmoothDrag;
    m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, s.bSeekbarSmoothDrag ? _T("Smooth drag seeking: on") : _T("Smooth drag seeking: off"), 1500);
    RefreshToolButtons();
}

void CPlayerSeekBar::OnUpdateSmoothDrag(CCmdUI* pCmdUI)
{
    pCmdUI->SetCheck(IsSmoothDrag());
}

void CPlayerSeekBar::OnExactDrag()
{
    auto& s = AfxGetAppSettings();
    s.bSeekbarExactDrag = !s.bSeekbarExactDrag;
    m_pMainFrame->m_OSD.DisplayMessage(OSD_TOPLEFT, s.bSeekbarExactDrag ? _T("Exact seeking: on (dragging and preview ignore keyframes)")
                                                                        : _T("Exact seeking: off (dragging and preview use keyframes)"), 2000);
    RefreshToolButtons();
}

void CPlayerSeekBar::OnUpdateExactDrag(CCmdUI* pCmdUI)
{
    pCmdUI->SetCheck(IsExactDrag());
}

void CPlayerSeekBar::OnZoomIn()
{
    ZoomBy(kCommandZoomStep);
}

void CPlayerSeekBar::OnZoomOut()
{
    ZoomBy(1.0 / kCommandZoomStep);
}

void CPlayerSeekBar::OnZoomReset()
{
    ResetZoom();
}

void CPlayerSeekBar::OnUpdateZoom(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(pCmdUI->m_nID == ID_SEEKBAR_ZOOM_IN ? m_bHasDuration : IsZoomed());
}
