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

#pragma once

#include "CMPCThemeToolTipCtrl.h"
#include <gdiplus.h>
#include <vector>

class CMainFrame;

// Colors shared by the LosslessCut strip and the seekbar. Segment colors follow LosslessCut:
// its 19-color palette indexed by segment number, adjusted with the same HSL formulas.
namespace llcui
{
    void EnsureGdiplus();

    struct Palette {
        bool dark;
        COLORREF bg;          // strip / seekbar background
        COLORREF channel;     // seekbar channel background
        COLORREF text;
        COLORREF textFade;
        COLORREF textDisabled;
        COLORREF hover;
        COLORREF pressed;
        COLORREF accent;
        COLORREF separator;
        COLORREF keyframe;    // lit keyframe buttons
        COLORREF ok, warn, error;
    };
    Palette GetPalette();

    COLORREF Blend(COLORREF fg, COLORREF bg, double alpha);
    Gdiplus::Color ToGdi(COLORREF c, BYTE alpha = 255);

    enum class SegState { Normal, Current, Excluded };
    COLORREF SegmentFill(int index, SegState state, const Palette& pal);     // already blended onto pal.channel
    COLORREF SegmentEdge(int index, const Palette& pal);                     // current segment's start/end edge
    COLORREF CutpointFill(int index, const Palette& pal);                    // set start/end buttons
    COLORREF CutpointBorder(int index, const Palette& pal);
    COLORREF SegmentDot(int index, const Palette& pal);

    void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
}

// The LosslessCut control row shown under the seekbar in LosslessCut mode. A child of the seekbar,
// so it shows, hides and auto-hides together with it. Everything is custom drawn.
class CLosslessCutStrip : public CWnd
{
public:
    explicit CLosslessCutStrip(CMainFrame* pMainFrame);

    BOOL Create(CWnd* pParent);
    int GetHeight() const;
    void Refresh();

private:
    enum class Btn {
        Launch, Info, PrevSegment, SetStart, PrevKeyframe, NextKeyframe, SetEnd, NextSegment,
        Add, Split, Remove, Undo, Redo, ScopeFull, ScopeSegments, ScopeSingle, Restrict, Loop, List, Status
    };
    struct Button {
        Btn id;
        CRect rc;
        bool visible = true;
        bool enabled = true;
        bool active = false;
    };

    CMainFrame* m_pMainFrame;
    std::vector<Button> m_buttons;
    int m_hot = -1;
    int m_pressed = -1;
    bool m_bTracking = false;
    CMPCThemeToolTipCtrl m_tooltip;
    CString m_tipText;
    CFont m_font, m_fontBold;
    int m_fontDpi = 0;
    CRect m_infoRect;
    int m_launchBleed = 0;              // the launch button's hover box reaches this far past its content
    CRect HitRect(const Button& b) const;
    std::vector<CRect> m_separators;

    void EnsureFonts();
    void Layout();
    void UpdateStates();
    void UpdateTooltips();
    int HitTest(CPoint pt) const;
    Button* Find(Btn id);
    UINT CommandFor(Btn id) const;
    CString TooltipFor(const Button& b) const;
    CString InfoText(CString& detail) const;
    void Execute(const Button& b);

    void DrawButton(Gdiplus::Graphics& g, CDC& dc, const Button& b, int index, const llcui::Palette& pal);
    void DrawIcon(Gdiplus::Graphics& g, Btn id, const Gdiplus::RectF& box, const Gdiplus::Color& color, const llcui::Palette& pal);

    DECLARE_MESSAGE_MAP()
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnMouseLeave();
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg int OnMouseActivate(CWnd* pDesktopWnd, UINT nHitTest, UINT message);
    afx_msg BOOL OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);
};
