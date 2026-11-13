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
#include "LosslessCutSegmentsBar.h"
#include "LosslessCutStrip.h"
#include "LosslessCut.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "CMPCTheme.h"
#include "CMPCThemeUtil.h"
#include <algorithm>

using namespace Gdiplus;

// -------------------------------------------------------------------------------------------------
// CLosslessCutNameEdit

BEGIN_MESSAGE_MAP(CLosslessCutNameEdit, CEdit)
    ON_WM_GETDLGCODE()
    ON_WM_KILLFOCUS()
END_MESSAGE_MAP()

BOOL CLosslessCutNameEdit::PreTranslateMessage(MSG* pMsg)
{
    if (pMsg->message == WM_KEYDOWN) {
        if (pMsg->wParam == VK_RETURN) {
            Finish(true);
            return TRUE;
        }
        if (pMsg->wParam == VK_ESCAPE) {
            Finish(false);
            return TRUE;
        }
    }
    if (pMsg->message == WM_KEYDOWN || pMsg->message == WM_KEYUP || pMsg->message == WM_CHAR) {
        // Keep typing away from the main window's accelerators.
        ::TranslateMessage(pMsg);
        ::DispatchMessage(pMsg);
        return TRUE;
    }
    return __super::PreTranslateMessage(pMsg);
}

UINT CLosslessCutNameEdit::OnGetDlgCode()
{
    return DLGC_WANTALLKEYS | __super::OnGetDlgCode();
}

void CLosslessCutNameEdit::OnKillFocus(CWnd* pNewWnd)
{
    __super::OnKillFocus(pNewWnd);
    Finish(true);
}

void CLosslessCutNameEdit::Finish(bool bCommit)
{
    if (m_bDone) {
        return;
    }
    m_bDone = true;
    if (onDone) {
        onDone(bCommit);
    }
    m_bDone = false;
}

// -------------------------------------------------------------------------------------------------
// CLosslessCutSegmentList

namespace
{
    CString SegmentLength(REFERENCE_TIME len)
    {
        CString s;
        if (len < 600000000LL) {
            s.Format(_T("%.2f s"), len / 10000000.0);
        } else {
            s = CLosslessCutController::FormatTime(len);
        }
        return s;
    }

    RectF ToRectF(const CRect& r)
    {
        return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top), static_cast<REAL>(r.Width()), static_cast<REAL>(r.Height()));
    }
}

CLosslessCutSegmentList::CLosslessCutSegmentList(CMainFrame* pMainFrame)
    : m_pMainFrame(pMainFrame)
{
}

BOOL CLosslessCutSegmentList::Create(CWnd* pParent)
{
    llcui::EnsureGdiplus();
    const CString cls = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    if (!CWnd::CreateEx(0, cls, _T(""), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, CRect(0, 0, 0, 0), pParent, 1)) {
        return FALSE;
    }
    m_tooltip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX);
    m_tooltip.SetMaxTipWidth(m_pMainFrame->m_dpi.ScaleX(360));
    TOOLINFO ti = { sizeof(TOOLINFO) };
    ti.hwnd = m_hWnd;
    ti.uId = 1;
    ti.uFlags = TTF_SUBCLASS;
    ti.lpszText = LPSTR_TEXTCALLBACK;
    m_tooltip.SendMessage(TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
    m_tooltip.Activate(TRUE);
    return TRUE;
}

void CLosslessCutSegmentList::EnsureFonts()
{
    const int dpi = m_pMainFrame->m_dpi.ScaleY(96);
    if (m_font.GetSafeHandle() && m_fontDpi == dpi) {
        return;
    }
    m_font.DeleteObject();
    m_fontBold.DeleteObject();
    m_fontSmall.DeleteObject();
    CMPCThemeUtil::getFontByType(m_font, this, CMPCThemeUtil::MessageFont);
    CMPCThemeUtil::getFontByType(m_fontBold, this, CMPCThemeUtil::MessageFont, false, true);
    LOGFONT lf;
    m_font.GetLogFont(&lf);
    lf.lfHeight = lf.lfHeight * 9 / 10;
    m_fontSmall.CreateFontIndirect(&lf);
    m_fontDpi = dpi;
}

int CLosslessCutSegmentList::HeaderHeight() const
{
    return m_pMainFrame->m_dpi.ScaleY(34);
}

int CLosslessCutSegmentList::RowHeight() const
{
    return m_pMainFrame->m_dpi.ScaleY(50);
}

int CLosslessCutSegmentList::ContentHeight() const
{
    return static_cast<int>(m_pMainFrame->m_llc.GetSegments().size()) * RowHeight() + m_pMainFrame->m_dpi.ScaleY(8);
}

int CLosslessCutSegmentList::MaxScroll() const
{
    CRect r;
    GetClientRect(r);
    return std::max(0, ContentHeight() - (r.Height() - HeaderHeight()));
}

void CLosslessCutSegmentList::ScrollTo(int y)
{
    const int clamped = std::clamp(y, 0, MaxScroll());
    if (clamped != m_scroll) {
        m_scroll = clamped;
        if (m_editRow >= 0) {
            EndRename(true);
        }
        Invalidate(FALSE);
    }
}

CRect CLosslessCutSegmentList::RowRect(int row) const
{
    CRect r;
    GetClientRect(r);
    const int top = HeaderHeight() + row * RowHeight() - m_scroll + m_pMainFrame->m_dpi.ScaleY(2);
    return CRect(r.left + m_pMainFrame->m_dpi.ScaleX(6), top, r.right - m_pMainFrame->m_dpi.ScaleX(12), top + RowHeight() - m_pMainFrame->m_dpi.ScaleY(4));
}

CRect CLosslessCutSegmentList::CheckRect(const CRect& row) const
{
    const int d = m_pMainFrame->m_dpi.ScaleX(18);
    const int x = row.right - m_pMainFrame->m_dpi.ScaleX(10) - d;
    const int y = row.top + (row.Height() - d) / 2;
    return CRect(x, y, x + d, y + d);
}

CRect CLosslessCutSegmentList::NameRect(const CRect& row) const
{
    const auto& dpi = m_pMainFrame->m_dpi;
    CRect r(row.left + dpi.ScaleX(46), row.top + row.Height() / 2, CheckRect(row).left - dpi.ScaleX(8), row.bottom - dpi.ScaleY(4));
    return r;
}

void CLosslessCutSegmentList::EnsureVisible(int row)
{
    if (row < 0) {
        return;
    }
    CRect client;
    GetClientRect(client);
    const int top = row * RowHeight();
    const int bottom = top + RowHeight();
    const int view = client.Height() - HeaderHeight();
    if (view <= RowHeight()) {
        return; // not laid out yet (or too small to scroll meaningfully)
    }
    if (top < m_scroll) {
        ScrollTo(top);
    } else if (bottom > m_scroll + view) {
        ScrollTo(bottom - view + m_pMainFrame->m_dpi.ScaleY(8));
    }
}

bool CLosslessCutSegmentList::ScrollbarRects(CRect& track, CRect& thumb) const
{
    CRect client;
    GetClientRect(client);
    const int view = client.Height() - HeaderHeight();
    const int content = ContentHeight();
    if (content <= view || view <= 0) {
        return false;
    }
    const auto& dpi = m_pMainFrame->m_dpi;
    track = CRect(client.right - dpi.ScaleX(8), HeaderHeight() + dpi.ScaleY(2), client.right - dpi.ScaleX(3), client.bottom - dpi.ScaleY(4));
    const int th = std::max(dpi.ScaleY(28), track.Height() * view / content);
    const int range = track.Height() - th;
    const int y = track.top + (MaxScroll() ? range * m_scroll / MaxScroll() : 0);
    thumb = CRect(track.left, y, track.right, y + th);
    return true;
}

int CLosslessCutSegmentList::RowAt(CPoint pt) const
{
    if (pt.y < HeaderHeight()) {
        return -1;
    }
    const int n = static_cast<int>(m_pMainFrame->m_llc.GetSegments().size());
    for (int i = 0; i < n; i++) {
        if (RowRect(i).PtInRect(pt)) {
            return i;
        }
    }
    return -1;
}

void CLosslessCutSegmentList::Refresh()
{
    if (!m_hWnd) {
        return;
    }
    const auto& llc = m_pMainFrame->m_llc;
    const int cur = llc.GetCurrentIndex();
    const size_t count = llc.GetSegments().size();
    if (m_editRow >= 0 && (count != m_lastCount || m_editRow >= static_cast<int>(count))) {
        EndRename(false); // the list changed under the editor
    }
    if (cur != m_lastCurrent || count != m_lastCount) {
        m_lastCurrent = cur;
        m_lastCount = count;
        m_scroll = std::min(m_scroll, MaxScroll());
        EnsureVisible(cur);
    }
    Invalidate(FALSE);
}

void CLosslessCutSegmentList::OnPaint()
{
    CPaintDC paintDC(this);
    CRect client;
    GetClientRect(client);
    if (client.IsRectEmpty()) {
        return;
    }
    EnsureFonts();
    const auto& dpi = m_pMainFrame->m_dpi;
    const auto& llc = m_pMainFrame->m_llc;
    const auto& segs = llc.GetSegments();
    const int cur = llc.GetCurrentIndex();
    const llcui::Palette pal = llcui::GetPalette();
    const bool canEdit = llc.CanEdit();

    CDC dc;
    dc.CreateCompatibleDC(&paintDC);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&paintDC, client.Width(), client.Height());
    CBitmap* oldBmp = dc.SelectObject(&bmp);
    dc.FillSolidRect(client, pal.bg);
    dc.SetBkMode(TRANSPARENT);

    // rows (clipped below the header)
    {
        Graphics g(dc.GetSafeHdc());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        // Rows that scroll under the header are covered by it (painted last); a GDI clip region
        // would not survive GDI+ drawing on the same DC.

        const float radius = static_cast<float>(dpi.ScaleX(7));
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            const CRect row = RowRect(i);
            if (row.bottom <= HeaderHeight() || row.top > client.bottom) {
                continue;
            }
            const auto& seg = segs[i];
            const bool isCur = (i == cur);
            const bool excluded = !seg.selected;
            const bool hot = (i == m_hotRow);

            // card
            COLORREF cardBg = pal.bg;
            if (isCur) {
                cardBg = llcui::Blend(llcui::SegmentDot(i, pal), pal.bg, pal.dark ? 0.17 : 0.13);
            } else if (hot) {
                cardBg = pal.hover;
            }
            if (cardBg != pal.bg) {
                GraphicsPath path;
                llcui::AddRoundRect(path, ToRectF(row), radius);
                SolidBrush b(llcui::ToGdi(cardBg));
                g.FillPath(&b, &path);
                if (isCur) {
                    Pen outline(llcui::ToGdi(llcui::Blend(llcui::SegmentDot(i, pal), pal.bg, 0.45)), 1.0f);
                    g.DrawPath(&outline, &path);
                }
            }

            // color bar on the left edge
            {
                const float w = static_cast<float>(dpi.ScaleX(isCur ? 4 : 3));
                const COLORREF c = excluded ? llcui::Blend(llcui::SegmentDot(i, pal), pal.bg, 0.4) : llcui::SegmentDot(i, pal);
                GraphicsPath bar;
                llcui::AddRoundRect(bar, RectF(row.left + static_cast<float>(dpi.ScaleX(5)), row.top + static_cast<float>(dpi.ScaleY(8)),
                                               w, row.Height() - static_cast<float>(dpi.ScaleY(16))), w / 2);
                SolidBrush b(llcui::ToGdi(c));
                g.FillPath(&b, &bar);
            }

            // number badge
            CRect badge(row.left + dpi.ScaleX(14), row.top + dpi.ScaleY(7), row.left + dpi.ScaleX(40), row.top + dpi.ScaleY(25));
            {
                const llcui::SegState st = isCur ? llcui::SegState::Current : excluded ? llcui::SegState::Excluded : llcui::SegState::Normal;
                GraphicsPath path;
                llcui::AddRoundRect(path, ToRectF(badge), static_cast<float>(dpi.ScaleX(5)));
                SolidBrush b(llcui::ToGdi(llcui::SegmentFill(i, st, pal)));
                g.FillPath(&b, &path);
                CString num;
                num.Format(_T("%d"), i + 1);
                CFont* old = dc.SelectObject(&m_fontBold);
                dc.SetTextColor(excluded && !isCur ? llcui::Blend(RGB(255, 255, 255), pal.bg, 0.7) : RGB(255, 255, 255));
                dc.DrawText(num, badge, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                dc.SelectObject(old);
            }

            // include-in-export toggle
            const CRect check = CheckRect(row);
            {
                const RectF cr = ToRectF(check);
                const bool hotCheck = hot && m_bHotCheck && canEdit;
                if (seg.selected) {
                    SolidBrush b(llcui::ToGdi(hotCheck ? llcui::Blend(RGB(255, 255, 255), pal.accent, 0.15) : pal.accent));
                    g.FillEllipse(&b, cr);
                    Pen p(Color(255, 255, 255, 255), 1.7f * dpi.ScaleX(100) / 100.0f);
                    p.SetStartCap(LineCapRound);
                    p.SetEndCap(LineCapRound);
                    p.SetLineJoin(LineJoinRound);
                    const float k = cr.Width / 16.0f;
                    PointF tick[] = { PointF(cr.X + 4.4f * k, cr.Y + 8.4f * k), PointF(cr.X + 6.9f * k, cr.Y + 10.8f * k), PointF(cr.X + 11.6f * k, cr.Y + 5.6f * k) };
                    g.DrawLines(&p, tick, 3);
                } else {
                    Pen p(llcui::ToGdi(hotCheck ? pal.text : pal.textDisabled), 1.5f);
                    RectF inner = cr;
                    inner.Inflate(-1.0f, -1.0f);
                    g.DrawEllipse(&p, inner);
                }
            }

            // texts
            const REFERENCE_TIME start = CLosslessCutController::ToRT(seg.start);
            CString times, length;
            if (seg.IsMarker()) {
                times = CLosslessCutController::FormatTime(start);
                length = _T("marker");
            } else {
                const REFERENCE_TIME end = CLosslessCutController::ToRT(*seg.end);
                times = CLosslessCutController::FormatTime(start) + _T("  \x2192  ") + CLosslessCutController::FormatTime(end);
                length = SegmentLength(end - start);
            }
            const COLORREF textMain = excluded ? pal.textFade : pal.text;
            CRect line1(row.left + dpi.ScaleX(46), row.top + dpi.ScaleY(4), check.left - dpi.ScaleX(8), row.top + row.Height() / 2 + dpi.ScaleY(2));
            CFont* old = dc.SelectObject(&m_fontSmall);
            const int lenW = dc.GetTextExtent(length).cx;
            const bool showLength = line1.Width() > lenW + dpi.ScaleX(150);
            if (showLength) {
                dc.SetTextColor(pal.textFade);
                dc.DrawText(length, line1, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                line1.right -= lenW + dpi.ScaleX(8);
            }
            dc.SelectObject(isCur ? &m_fontBold : &m_font);
            dc.SetTextColor(textMain);
            dc.DrawText(times, line1, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

            if (i != m_editRow) {
                CRect line2 = NameRect(row);
                dc.SelectObject(&m_fontSmall);
                CString name = CString(llc::Utf8ToWide(seg.name).c_str());
                if (name.IsEmpty()) {
                    dc.SetTextColor(pal.textDisabled);
                    if (seg.IsMarker()) {
                        name = _T("No end yet \x2013 press O to set it");
                    } else if (excluded) {
                        name = _T("Excluded from the export");
                    } else if (hot && canEdit && !m_bHotCheck) {
                        name = _T("Double-click to name");
                    }
                } else {
                    dc.SetTextColor(excluded ? pal.textDisabled : pal.textFade);
                    if (excluded) {
                        name += _T("  \x00b7  excluded");
                    }
                }
                dc.DrawText(name, line2, DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            dc.SelectObject(old);
        }

        // scrollbar
        CRect track, thumb;
        if (ScrollbarRects(track, thumb)) {
            GraphicsPath path;
            llcui::AddRoundRect(path, ToRectF(thumb), thumb.Width() / 2.0f);
            SolidBrush b(llcui::ToGdi(m_bDraggingThumb ? pal.textFade : llcui::Blend(pal.text, pal.bg, 0.28)));
            g.FillPath(&b, &path);
        }
    }

    // header: totals for what LosslessCut will export (opaque, over any row scrolled under it)
    {
        dc.FillSolidRect(0, 0, client.Width(), HeaderHeight(), pal.bg);
        REFERENCE_TIME total = 0;
        int included = 0;
        for (const auto& s : segs) {
            if (s.selected && s.end) {
                total += CLosslessCutController::ToRT(*s.end) - CLosslessCutController::ToRT(s.start);
                included++;
            }
        }
        CRect hr(dpi.ScaleX(12), 0, client.right - dpi.ScaleX(12), HeaderHeight());
        CFont* old = dc.SelectObject(&m_fontBold);
        CString totalText = CLosslessCutController::FormatTime(total);
        dc.SetTextColor(pal.text);
        dc.DrawText(totalText, hr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        const int tw = dc.GetTextExtent(totalText).cx;
        dc.SelectObject(&m_fontSmall);
        CString summary;
        if (segs.empty()) {
            summary = _T("No segments");
        } else {
            summary.Format(_T("%d of %d in the export"), included, static_cast<int>(segs.size()));
        }
        dc.SetTextColor(pal.textFade);
        CRect sr(hr);
        sr.right -= tw + dpi.ScaleX(10);
        dc.DrawText(summary, sr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        dc.SelectObject(old);
        dc.FillSolidRect(dpi.ScaleX(8), HeaderHeight() - 1, client.Width() - dpi.ScaleX(16), 1, pal.separator);
    }

    if (segs.empty()) {
        CRect er(client.left + dpi.ScaleX(16), HeaderHeight() + dpi.ScaleY(24), client.right - dpi.ScaleX(16), client.bottom);
        CFont* old = dc.SelectObject(&m_font);
        dc.SetTextColor(pal.textFade);
        dc.DrawText(_T("Press I at the start of a part you want to keep and O at its end.\n\nThe segments show up here and on the timeline, and are saved for LosslessCut right away."),
                    er, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
        dc.SelectObject(old);
    }

    paintDC.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
    dc.SelectObject(oldBmp);
}

BOOL CLosslessCutSegmentList::OnEraseBkgnd(CDC* /*pDC*/)
{
    return TRUE;
}

void CLosslessCutSegmentList::BeginRename(int row)
{
    const auto& llc = m_pMainFrame->m_llc;
    if (!llc.CanEdit() || row < 0 || row >= static_cast<int>(llc.GetSegments().size())) {
        return;
    }
    EndRename(true);
    EnsureVisible(row);
    m_editRow = row;
    CRect r = NameRect(RowRect(row));
    r.InflateRect(m_pMainFrame->m_dpi.ScaleX(3), m_pMainFrame->m_dpi.ScaleY(2));
    if (!m_edit.m_hWnd) {
        m_edit.Create(WS_CHILD | ES_AUTOHSCROLL | WS_BORDER, r, this, 2);
        m_edit.onDone = [this](bool bCommit) { EndRename(bCommit); };
    }
    EnsureFonts();
    m_edit.SetFont(&m_fontSmall);
    m_edit.SetWindowText(CString(llc::Utf8ToWide(llc.GetSegments()[row].name).c_str()));
    m_edit.MoveWindow(r);
    m_edit.ShowWindow(SW_SHOW);
    m_edit.SetFocus();
    m_edit.SetSel(0, -1);
    Invalidate(FALSE);
}

void CLosslessCutSegmentList::EndRename(bool bCommit)
{
    if (m_editRow < 0) {
        return;
    }
    const int row = m_editRow;
    m_editRow = -1;
    CString text;
    m_edit.GetWindowText(text);
    m_edit.ShowWindow(SW_HIDE);
    if (GetFocus() == &m_edit || !GetFocus()) {
        m_pMainFrame->SetFocus();
    }
    if (bCommit) {
        m_pMainFrame->m_llc.RenameSegment(row, text);
    }
    Invalidate(FALSE);
}

HBRUSH CLosslessCutSegmentList::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    const llcui::Palette pal = llcui::GetPalette();
    if (pWnd == &m_edit) {
        const COLORREF bg = llcui::Blend(pal.text, pal.bg, pal.dark ? 0.08 : 0.0);
        m_editBrush.DeleteObject();
        m_editBrush.CreateSolidBrush(bg);
        pDC->SetTextColor(pal.text);
        pDC->SetBkColor(bg);
        return m_editBrush;
    }
    return __super::OnCtlColor(pDC, pWnd, nCtlColor);
}

BEGIN_MESSAGE_MAP(CLosslessCutSegmentList, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_MOUSEWHEEL()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_RBUTTONUP()
    ON_WM_CAPTURECHANGED()
    ON_WM_CTLCOLOR()
    ON_NOTIFY_EX_RANGE(TTN_NEEDTEXT, 0, 0xFFFF, OnToolTipText)
END_MESSAGE_MAP()

void CLosslessCutSegmentList::OnSize(UINT nType, int cx, int cy)
{
    __super::OnSize(nType, cx, cy);
    m_scroll = std::clamp(m_scroll, 0, MaxScroll());
    EnsureVisible(m_pMainFrame->m_llc.GetCurrentIndex());
    if (m_editRow >= 0) {
        EndRename(true);
    }
    if (m_tooltip.m_hWnd) {
        TOOLINFO ti = { sizeof(TOOLINFO) };
        ti.hwnd = m_hWnd;
        ti.uId = 1;
        GetClientRect(&ti.rect);
        m_tooltip.SendMessage(TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
    }
    Invalidate(FALSE);
}

BOOL CLosslessCutSegmentList::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    UNREFERENCED_PARAMETER(nFlags);
    UNREFERENCED_PARAMETER(pt);
    ScrollTo(m_scroll - zDelta * RowHeight() / WHEEL_DELTA);
    return TRUE;
}

void CLosslessCutSegmentList::OnMouseMove(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    if (m_bDraggingThumb) {
        CRect track, thumb;
        if (ScrollbarRects(track, thumb)) {
            const int range = track.Height() - thumb.Height();
            if (range > 0) {
                ScrollTo(m_dragStartScroll + (point.y - m_dragStartY) * MaxScroll() / range);
            }
        }
        return;
    }
    if (!m_bTracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
        m_bTracking = !!TrackMouseEvent(&tme);
    }
    const int row = RowAt(point);
    const bool onCheck = row >= 0 && CheckRect(RowRect(row)).PtInRect(point);
    if (row != m_hotRow || onCheck != m_bHotCheck) {
        if (row != m_hotRow) {
            m_tooltip.Pop(); // a different row: new tooltip text
        }
        m_hotRow = row;
        m_bHotCheck = onCheck;
        Invalidate(FALSE);
    }
}

void CLosslessCutSegmentList::OnMouseLeave()
{
    m_bTracking = false;
    if (m_hotRow != -1) {
        m_hotRow = -1;
        m_bHotCheck = false;
        Invalidate(FALSE);
    }
}

void CLosslessCutSegmentList::OnLButtonDown(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    CRect track, thumb;
    if (ScrollbarRects(track, thumb)) {
        CRect hit(track);
        hit.InflateRect(m_pMainFrame->m_dpi.ScaleX(4), 0);
        if (hit.PtInRect(point)) {
            if (thumb.PtInRect(point)) {
                m_bDraggingThumb = true;
                m_dragStartY = point.y;
                m_dragStartScroll = m_scroll;
                SetCapture();
            } else {
                ScrollTo(m_scroll + (point.y < thumb.top ? -1 : 1) * (track.Height()));
            }
            Invalidate(FALSE);
            return;
        }
    }
    const int row = RowAt(point);
    if (row < 0) {
        return;
    }
    auto& llc = m_pMainFrame->m_llc;
    if (CheckRect(RowRect(row)).PtInRect(point)) {
        llc.ToggleSelected(row);
        return;
    }
    llc.SelectSegment(row, true);
}

void CLosslessCutSegmentList::OnLButtonUp(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    UNREFERENCED_PARAMETER(point);
    if (m_bDraggingThumb) {
        m_bDraggingThumb = false;
        ReleaseCapture();
        Invalidate(FALSE);
    }
}

void CLosslessCutSegmentList::OnLButtonDblClk(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    const int row = RowAt(point);
    if (row >= 0 && !CheckRect(RowRect(row)).PtInRect(point)) {
        BeginRename(row);
    }
}

void CLosslessCutSegmentList::OnRButtonUp(UINT nFlags, CPoint point)
{
    UNREFERENCED_PARAMETER(nFlags);
    auto& llc = m_pMainFrame->m_llc;
    const int row = RowAt(point);
    REFERENCE_TIME at = m_pMainFrame->GetPos();
    if (row >= 0) {
        llc.SelectSegment(row, false);
        const auto& seg = llc.GetSegments()[row];
        at = CLosslessCutController::ToRT(seg.start);
        if (seg.end) {
            at += (CLosslessCutController::ToRT(*seg.end) - at) / 2; // a point inside the segment
        }
    }
    ClientToScreen(&point);
    llc.ShowContextMenu(this, point, at);
}

void CLosslessCutSegmentList::OnCaptureChanged(CWnd* pWnd)
{
    __super::OnCaptureChanged(pWnd);
    if (pWnd != this && m_bDraggingThumb) {
        m_bDraggingThumb = false;
        Invalidate(FALSE);
    }
}

BOOL CLosslessCutSegmentList::OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult)
{
    UNREFERENCED_PARAMETER(id);
    if (pNMHDR->hwndFrom != m_tooltip.m_hWnd) {
        return FALSE;
    }
    auto* pTTT = reinterpret_cast<NMTTDISPINFO*>(pNMHDR);
    const auto& llc = m_pMainFrame->m_llc;
    m_tipText.Empty();
    if (m_hotRow >= 0 && m_hotRow < static_cast<int>(llc.GetSegments().size())) {
        if (m_bHotCheck) {
            m_tipText = llc.GetSegments()[m_hotRow].selected ? _T("Included in the export \x2013 click to exclude")
                                                             : _T("Excluded from the export \x2013 click to include");
        } else {
            m_tipText = _T("Click to go to this segment\nDouble-click to rename it\nRight-click for more");
        }
    }
    pTTT->lpszText = const_cast<LPTSTR>(m_tipText.GetString());
    *pResult = 0;
    return TRUE;
}

// -------------------------------------------------------------------------------------------------
// CLosslessCutSegmentsBar

IMPLEMENT_DYNAMIC(CLosslessCutSegmentsBar, CMPCThemePlayerBar)

CLosslessCutSegmentsBar::CLosslessCutSegmentsBar(CMainFrame* pMainFrame)
    : CMPCThemePlayerBar(pMainFrame)
    , m_list(pMainFrame)
{
}

BOOL CLosslessCutSegmentsBar::Create(CWnd* pParentWnd, UINT defDockBarID)
{
    if (!CMPCThemePlayerBar::Create(ResStr(IDS_LLC_SEGMENTS_CAPTION), pParentWnd, ID_LLC_TOGGLE_SEGMENT_LIST, defDockBarID, _T("LosslessCut Segments"))) {
        return FALSE;
    }
    if (!m_list.Create(this)) {
        return FALSE;
    }
    const auto& dpi = m_pMainFrame->m_dpi;
    m_szMinVert = CSize(dpi.ScaleX(200), dpi.ScaleY(120));
    m_szMinHorz = CSize(dpi.ScaleX(200), dpi.ScaleY(120));
    m_szMinFloat = CSize(dpi.ScaleX(220), dpi.ScaleY(160));
    m_szFloat = CSize(dpi.ScaleX(300), dpi.ScaleY(420));
    return TRUE;
}

void CLosslessCutSegmentsBar::Refresh()
{
    if (m_hWnd && IsWindowVisible()) {
        m_list.Refresh();
    }
}

void CLosslessCutSegmentsBar::ReloadTranslatableResources()
{
    SetWindowText(ResStr(IDS_LLC_SEGMENTS_CAPTION));
}

void CLosslessCutSegmentsBar::LoadState(CFrameWnd* pParent)
{
    __super::LoadState(pParent);
    // Restores size and dock position only; it is shown by LosslessCut mode when a file has a project.
    if (IsWindowVisible()) {
        m_pMainFrame->ShowControlBar(this, FALSE, FALSE);
    }
}

BEGIN_MESSAGE_MAP(CLosslessCutSegmentsBar, CMPCThemePlayerBar)
    ON_WM_SIZE()
    ON_WM_WINDOWPOSCHANGED()
END_MESSAGE_MAP()

void CLosslessCutSegmentsBar::OnSize(UINT nType, int cx, int cy)
{
    __super::OnSize(nType, cx, cy);
    if (m_list.m_hWnd) {
        CRect r;
        GetClientRect(r);
        m_list.MoveWindow(r);
    }
}

void CLosslessCutSegmentsBar::OnWindowPosChanged(WINDOWPOS* lpwndpos)
{
    __super::OnWindowPosChanged(lpwndpos);
    auto& llc = m_pMainFrame->m_llc;
    if (lpwndpos->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) {
        // The panel's own close button: remember the choice for the next file.
        if (llc.IsActive() && !llc.IsSyncingSegmentList() && !IsAutohidden()) {
            AfxGetAppSettings().bLosslessCutShowList = !!(lpwndpos->flags & SWP_SHOWWINDOW);
        }
        if (lpwndpos->flags & SWP_SHOWWINDOW) {
            m_list.Refresh();
        }
        llc.OnSegmentListShownOrHidden();
    }
}
