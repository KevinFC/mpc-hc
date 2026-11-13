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
#include "SeekBarToolButtons.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "CMPCTheme.h"
#include <cmath>

using namespace Gdiplus;
using SeekTool = CPlayerSeekBar::SeekTool;

namespace
{
    const SeekTool kTools[] = { SeekTool::Smooth, SeekTool::Exact, SeekTool::Zoom };

    // The glyphs, drawn on a 16x16 grid scaled to the box.
    void DrawGlyph(Graphics& g, SeekTool tool, const RectF& box, COLORREF fg, bool bActive)
    {
        const REAL k = box.Width / 16.0f;
        auto P = [&](REAL x, REAL y) { return PointF(box.X + x * k, box.Y + y * k); };
        const Color color = llcui::ToGdi(fg);
        Pen pen(color, std::max(1.2f, 1.6f * k));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        SolidBrush brush(color);

        switch (tool) {
            case SeekTool::Smooth: {
                // an ease-in-out curve over a baseline, with the playhead arriving at its end
                Pen faint(llcui::ToGdi(fg, 100), std::max(1.0f, 1.1f * k));
                g.DrawLine(&faint, P(1.0f, 14.5f), P(15.0f, 14.5f));
                g.DrawBezier(&pen, P(1.5f, 12.0f), P(10.5f, 12.0f), P(4.5f, 3.0f), P(12.5f, 3.0f));
                const REAL d = 4.2f * k;
                const PointF end = P(12.5f, 3.0f);
                g.FillEllipse(&brush, end.X - d / 2, end.Y - d / 2, d, d);
                break;
            }
            case SeekTool::Exact: {
                // a key, struck through: dragging doesn't stop at keyframes
                const PointF c = P(5.0f, 6.0f);
                const REAL r = 3.2f * k;
                g.DrawEllipse(&pen, c.X - r, c.Y - r, r * 2, r * 2);
                g.DrawLine(&pen, P(7.3f, 8.3f), P(13.0f, 14.0f));
                g.DrawLine(&pen, P(10.6f, 11.6f), P(12.4f, 9.8f));
                g.DrawLine(&pen, P(12.4f, 13.4f), P(14.0f, 11.8f));
                Pen slash(color, std::max(1.4f, 1.8f * k));
                slash.SetStartCap(LineCapRound);
                slash.SetEndCap(LineCapRound);
                g.DrawLine(&slash, P(14.5f, 1.5f), P(1.5f, 14.5f));
                break;
            }
            case SeekTool::Zoom: {
                // a magnifier; minus while zoomed in (clicking shows the whole timeline again)
                const PointF c = P(6.6f, 6.6f);
                const REAL r = 5.0f * k;
                g.DrawEllipse(&pen, c.X - r, c.Y - r, r * 2, r * 2);
                Pen handle(color, std::max(1.6f, 2.3f * k));
                handle.SetStartCap(LineCapRound);
                handle.SetEndCap(LineCapRound);
                g.DrawLine(&handle, P(10.4f, 10.4f), P(14.6f, 14.6f));
                g.DrawLine(&pen, P(4.3f, 6.6f), P(8.9f, 6.6f));
                if (!bActive) {
                    g.DrawLine(&pen, P(6.6f, 4.3f), P(6.6f, 8.9f));
                }
                break;
            }
        }
    }
}

CSeekBarToolButtons::CSeekBarToolButtons(CMainFrame* pMainFrame)
    : m_pMainFrame(pMainFrame)
{
}

BOOL CSeekBarToolButtons::Create(CWnd* pParent)
{
    llcui::EnsureGdiplus();
    const CString cls = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    if (!CWnd::CreateEx(0, cls, _T(""), WS_CHILD | WS_CLIPSIBLINGS, CRect(0, 0, 0, 0), pParent, 0)) {
        return FALSE;
    }
    m_tooltip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX);
    m_tooltip.SetMaxTipWidth(m_pMainFrame->m_dpi.ScaleX(420));
    m_tooltip.Activate(TRUE);
    return TRUE;
}

int CSeekBarToolButtons::GetGroupWidth(int buttonWidth) const
{
    return AfxGetAppSettings().bSeekbarToolButtons ? buttonWidth * kCount : 0;
}

void CSeekBarToolButtons::Place(const CRect& rc, int buttonWidth)
{
    if (!m_hWnd) {
        return;
    }
    m_buttonWidth = buttonWidth;
    if (rc.IsRectEmpty()) {
        if (IsWindowVisible()) {
            ShowWindow(SW_HIDE);
        }
        return;
    }
    SetWindowPos(nullptr, rc.left, rc.top, rc.Width(), rc.Height(), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateTooltips();
    Invalidate(FALSE);
}

unsigned CSeekBarToolButtons::CurrentState() const
{
    const auto& bar = m_pMainFrame->m_wndSeekBar;
    return (bar.IsSmoothDrag() ? 1u : 0u) | (bar.IsExactDrag() ? 2u : 0u) | (bar.IsZoomed() ? 4u : 0u) |
           (bar.HasDuration() ? 8u : 0u) | (AppIsThemeLoaded() ? 16u : 0u);
}

void CSeekBarToolButtons::SyncState()
{
    const unsigned state = CurrentState();
    if (state != m_state && m_hWnd) {
        m_state = state;
        Invalidate(FALSE);
        m_tooltip.SendMessage(TTM_UPDATE, 0, 0); // a tooltip that is showing picks up the change
    }
}

CRect CSeekBarToolButtons::ButtonRect(int i) const
{
    CRect client;
    GetClientRect(&client);
    return CRect(client.left + i * m_buttonWidth, client.top, client.left + (i + 1) * m_buttonWidth, client.bottom);
}

int CSeekBarToolButtons::HitTest(CPoint pt) const
{
    for (int i = 0; i < kCount; i++) {
        if (ButtonRect(i).PtInRect(pt)) {
            return i;
        }
    }
    return -1;
}

void CSeekBarToolButtons::UpdateTooltips()
{
    if (!m_tooltip.m_hWnd) {
        return;
    }
    // registered directly with TTF_SUBCLASS (MFC's AddTool(rect) leaves it out)
    for (int i = 0; i < kCount; i++) {
        TOOLINFO ti = { sizeof(TOOLINFO) };
        ti.hwnd = m_hWnd;
        ti.uId = i + 1;
        ti.rect = ButtonRect(i);
        if (m_tooltip.SendMessage(TTM_GETTOOLINFO, 0, reinterpret_cast<LPARAM>(&ti))) {
            ti.rect = ButtonRect(i);
            m_tooltip.SendMessage(TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
        } else {
            ti.uFlags = TTF_SUBCLASS;
            ti.lpszText = LPSTR_TEXTCALLBACK;
            m_tooltip.SendMessage(TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
}

BEGIN_MESSAGE_MAP(CSeekBarToolButtons, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_RBUTTONUP()
    ON_WM_MOUSEWHEEL()
    ON_WM_CAPTURECHANGED()
    ON_WM_SETCURSOR()
    ON_NOTIFY_EX_RANGE(TTN_NEEDTEXT, 0, 0xFFFF, OnToolTipText)
END_MESSAGE_MAP()

void CSeekBarToolButtons::OnPaint()
{
    CPaintDC paintDC(this);
    CRect client;
    GetClientRect(&client);
    if (client.IsRectEmpty()) {
        return;
    }
    m_state = CurrentState();
    const bool bTheme = AppIsThemeLoaded();
    const llcui::Palette pal = llcui::GetPalette();
    const COLORREF bg = bTheme ? CMPCTheme::PlayerBGColor : GetSysColor(COLOR_BTNFACE);

    CDC dc;
    dc.CreateCompatibleDC(&paintDC);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&paintDC, client.Width(), client.Height());
    CBitmap* oldBmp = dc.SelectObject(&bmp);
    dc.FillSolidRect(client, bg);

    const auto& bar = m_pMainFrame->m_wndSeekBar;
    for (int i = 0; i < kCount; i++) {
        const SeekTool tool = kTools[i];
        const bool bEnabled = tool != SeekTool::Zoom || (m_state & 8u);
        const bool bActive = tool == SeekTool::Smooth ? bar.IsSmoothDrag() : tool == SeekTool::Exact ? bar.IsExactDrag() : bar.IsZoomed();
        const bool bHot = bEnabled && m_hot == i;
        const bool bPressed = bHot && m_pressed == i;
        const CRect rc = ButtonRect(i);

        // the toolbar's own button look: a filled box with a thin border
        COLORREF fill = bg;
        bool bFill = true;
        if (bTheme) {
            fill = bPressed ? CMPCTheme::PlayerButtonClickedColor : bActive ? CMPCTheme::PlayerButtonCheckedColor : bHot ? CMPCTheme::PlayerButtonHotColor : bg;
            bFill = bPressed || bActive || bHot;
        } else {
            fill = bPressed ? llcui::Blend(pal.text, bg, 0.2) : bActive ? llcui::Blend(pal.accent, bg, 0.22) : bHot ? llcui::Blend(pal.text, bg, 0.1) : bg;
            bFill = bPressed || bActive || bHot;
        }
        if (bFill) {
            dc.FillSolidRect(rc, fill);
            CBrush border(bTheme ? CMPCTheme::PlayerButtonBorderColor : GetSysColor(COLOR_3DSHADOW));
            dc.FrameRect(rc, &border);
        }

        COLORREF fg = !bEnabled ? pal.textDisabled : bActive ? pal.accent : llcui::Blend(pal.text, bg, bHot ? 1.0 : 0.82);
        if (bActive && bTheme) {
            fg = llcui::Blend(pal.accent, RGB(255, 255, 255), 0.75); // a little lighter, to read on the checked fill
        }
        const REAL size = static_cast<REAL>(std::min(rc.Width(), rc.Height()) - m_pMainFrame->m_dpi.ScaleY(8));
        const RectF box(rc.left + (rc.Width() - size) / 2.0f, rc.top + (rc.Height() - size) / 2.0f, size, size);
        Graphics g(dc.GetSafeHdc());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        DrawGlyph(g, tool, box, fg, bActive);
    }

    paintDC.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
    dc.SelectObject(oldBmp);
}

BOOL CSeekBarToolButtons::OnEraseBkgnd(CDC* /*pDC*/)
{
    return TRUE;
}

void CSeekBarToolButtons::OnMouseMove(UINT nFlags, CPoint point)
{
    __super::OnMouseMove(nFlags, point);
    if (!m_bTracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
        m_bTracking = !!TrackMouseEvent(&tme);
    }
    const int hot = HitTest(point);
    if (hot != m_hot) {
        m_hot = hot;
        Invalidate(FALSE);
    }
}

void CSeekBarToolButtons::OnMouseLeave()
{
    m_bTracking = false;
    if (m_hot != -1) {
        m_hot = -1;
        Invalidate(FALSE);
    }
}

void CSeekBarToolButtons::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    const int i = HitTest(point);
    if (i >= 0) {
        m_pressed = m_hot = i;
        SetCapture();
        Invalidate(FALSE);
    }
}

void CSeekBarToolButtons::OnLButtonUp(UINT /*nFlags*/, CPoint point)
{
    const int pressed = m_pressed;
    m_pressed = -1;
    if (GetCapture() == this) {
        ReleaseCapture();
    }
    Invalidate(FALSE);
    if (pressed >= 0 && HitTest(point) == pressed) {
        m_pMainFrame->m_wndSeekBar.OnSeekToolClicked(kTools[pressed]);
        m_pMainFrame->RestoreFocus();
    }
}

void CSeekBarToolButtons::OnLButtonDblClk(UINT nFlags, CPoint point)
{
    OnLButtonDown(nFlags, point);
}

void CSeekBarToolButtons::OnRButtonUp(UINT /*nFlags*/, CPoint point)
{
    const int i = HitTest(point);
    if (i >= 0) {
        ClientToScreen(&point);
        m_pMainFrame->m_wndSeekBar.ShowSeekToolMenu(kTools[i], point, this);
    }
}

BOOL CSeekBarToolButtons::OnMouseWheel(UINT /*nFlags*/, short zDelta, CPoint pt)
{
    CPoint point(pt);
    ScreenToClient(&point);
    auto& bar = m_pMainFrame->m_wndSeekBar;
    const int i = HitTest(point);
    if (i >= 0 && kTools[i] == SeekTool::Smooth) {
        m_wheelAccum += zDelta;
        const int steps = m_wheelAccum / WHEEL_DELTA;
        if (steps != 0) {
            m_wheelAccum -= steps * WHEEL_DELTA;
            bar.SetSmoothStrength(AfxGetAppSettings().iSeekbarSmoothStrength + steps);
        }
    } else if (i >= 0 && kTools[i] == SeekTool::Zoom) {
        bar.ZoomBy(std::pow(1.5, zDelta / static_cast<double>(WHEEL_DELTA)));
    }
    return TRUE;
}

void CSeekBarToolButtons::OnCaptureChanged(CWnd* pWnd)
{
    __super::OnCaptureChanged(pWnd);
    if (pWnd != this && m_pressed >= 0) {
        m_pressed = -1;
        Invalidate(FALSE);
    }
}

BOOL CSeekBarToolButtons::OnSetCursor(CWnd* /*pWnd*/, UINT /*nHitTest*/, UINT /*message*/)
{
    ::SetCursor(AfxGetApp()->LoadStandardCursor(IDC_HAND)); // like the toolbar's buttons
    return TRUE;
}

BOOL CSeekBarToolButtons::OnToolTipText(UINT /*id*/, NMHDR* pNMHDR, LRESULT* pResult)
{
    const int i = static_cast<int>(pNMHDR->idFrom) - 1;
    if (pNMHDR->hwndFrom != m_tooltip.m_hWnd || i < 0 || i >= kCount) {
        return FALSE;
    }
    auto* pTTT = reinterpret_cast<NMTTDISPINFO*>(pNMHDR);
    m_tipText = m_pMainFrame->m_wndSeekBar.SeekToolTip(kTools[i]);
    pTTT->lpszText = const_cast<LPTSTR>(m_tipText.GetString());
    pTTT->hinst = nullptr;
    *pResult = 0;
    return TRUE;
}
