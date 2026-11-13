/*
 * (C) 2003-2006 Gabest
 * (C) 2006-2018 see Authors.txt
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

#include "EventDispatcher.h"
#include "DSMPropertyBag.h"
#include <memory>
#include "CMPCThemeToolTipCtrl.h"
#include "CMPCThemeMenu.h"
#include "LosslessCutStrip.h"

class CMainFrame;
enum class SeekMode;

class CPlayerSeekBar : public CDialogBar
{
    DECLARE_DYNAMIC(CPlayerSeekBar)

public:
    CPlayerSeekBar(CMainFrame* pMainFrame);
    virtual ~CPlayerSeekBar();
    virtual BOOL Create(CWnd* pParentWnd);

private:
    enum { TIMER_SHOWHIDE_TOOLTIP = 1, TIMER_PREVIEW_SEEK, TIMER_ANIMATE };

    CMainFrame* m_pMainFrame;
    REFERENCE_TIME m_rtStart, m_rtStop, m_rtPos, m_rtPosDraw;
	REFERENCE_TIME m_pos_preview = 0;
    LONG m_last_pointx_preview = 0;
    ULONGLONG m_lastPreviewSeekTick = 0;

    bool m_bEnabled;
    bool m_bHasDuration;
    REFERENCE_TIME m_rtHoverPos;
    CPoint m_hoverPoint;
    HCURSOR m_cursor;
    bool m_bDraggingThumb, m_bHoverThumb;
    bool pauseAfterFirstScroll = false;
    bool pausedForScrolling = false;

    EventClient m_eventc;
    void EventCallback(MpcEvent ev);

    CMPCThemeToolTipCtrl m_tooltip;
    enum { TOOLTIP_HIDDEN, TOOLTIP_TRIGGERED, TOOLTIP_VISIBLE } m_tooltipState;
    CFont mpcThemeFont;
    TOOLINFO m_ti;
    CPoint m_tooltipPoint;
    bool m_bIgnoreLastTooltipPoint;
    CString m_tooltipText;

    CComPtr<IDSMChapterBag> m_pChapterBag;
    CCritSec m_csChapterBag; // Graph thread sets the chapter bag

    std::unique_ptr<CDC> m_pEnabledThumb;
    std::unique_ptr<CDC> m_pDisabledThumb;
    CRect m_lastThumbRect;

    // LosslessCut mode: segments drawn in the channel, and the control strip below it
    CLosslessCutStrip m_llcStrip;
    bool m_bLlcMode = false;
    CFont m_llcLabelFont;
    int m_llcLabelFontHeight = 0;
    int LlcStripHeight() const;
    bool DrawsLosslessCutChannel() const;
    void DrawLosslessCutChannel(CDC& dc, const CRect& channelRect);
    void PositionLlcStrip();

    // Zoom (PlayerSeekBarTools.cpp). The channel shows the view [m_viewStart, m_viewStart + m_viewSpan];
    // the target view is where it animates to. A span of 0 means the whole range.
    double m_viewStart = 0.0, m_viewSpan = 0.0;
    double m_viewTargetStart = 0.0, m_viewTargetSpan = 0.0;
    bool m_bViewAnchor = false;         // zooming keeps m_anchorTime at m_anchorFrac of the width while animating
    double m_anchorTime = 0.0, m_anchorFrac = 0.0;
    ULONGLONG m_lastUserPanTick = 0;
    enum class OverviewHit { None, Label, Track, Window, LeftEdge, RightEdge };
    OverviewHit m_overviewHot = OverviewHit::None;
    bool m_bPanning = false;            // dragging the overview window, or one of its ends
    OverviewHit m_panPart = OverviewHit::None;
    double m_panGrab = 0.0;             // where in the window it was grabbed (time from the view start)
    double m_lastAnimMs = 0.0;
    bool m_bAnimating = false;
    int m_lastOverviewX = INT_MIN;      // where the overview's playhead was drawn
    CFont m_overviewFont;
    int m_overviewFontHeight = 0;

    // Smooth drag seeking: while the button is down, the thumb glides toward the mouse.
    bool m_bGliding = false;
    double m_glidePos = 0.0;
    REFERENCE_TIME m_glideTarget = 0;
    CPoint m_glidePoint;

    bool m_bTrackingLeave = false;
    CMPCThemeToolTipCtrl m_toolTip;     // the zoom overview's tooltips
    CString m_toolTipText;
    CMPCThemeMenu m_toolMenu;
    CString ToolTipFor(UINT_PTR id);
    void UpdateToolTips();
    void RefreshToolButtons();          // the buttons on the controls toolbar show these states

    REFERENCE_TIME ViewStart() const;
    REFERENCE_TIME ViewStop() const;
    double ViewSpan() const;
    double MinViewSpan() const;
    bool InView(REFERENCE_TIME rt) const;
    int OverviewHeight() const;
    CRect GetOverviewRect() const;      // the track, lined up with the channel
    CRect GetOverviewWindowRect() const;
    CRect GetOverviewLabelRect() const;
    OverviewHit OverviewHitTest(CPoint point) const;
    double OverviewTimeAt(int x) const;
    void DragOverview(int x);
    void EndOverviewDrag();
    void DrawOverview(CDC& dc);
    void SetViewTarget(double start, double span, bool bAnimate);
    void ZoomAround(double factor, double anchorTime, double anchorFrac);
    void PanTo(double start);
    void FollowPlayhead(REFERENCE_TIME rtOld, REFERENCE_TIME rtNew);
    void OnZoomStateMaybeChanged(bool bWasZoomed);
    void StartAnimation();
    void OnAnimate();

    void MoveThumbToTime(REFERENCE_TIME rtPosDraw);
    void SetGlideTarget(CPoint point);
    void FinishDrag();

    CString m_timeText;                 // currently displayed time string (empty when none)
    mutable CFont m_timeFont;           // font for the time text
    mutable int   m_timeFontHeight = 0; // char height the cached font was built for (px)
    mutable int   m_timeReservedWidth = 0; // modeled (worst-case-for-options) time-section width, the stable floor (px)
    mutable int   m_timeActualWidth = 0;   // width of the actual current time string; widens the section if it ever exceeds the model (px)
    mutable CString m_timeTemplate;     // widest string the reserved width was measured for (tracks the active options)

    bool ShowTimeOnSeekBar() const;
    bool TimeSectionOnLeft() const;     // true => the time section sits left of the channel (advanced option)
    bool TimeSectionVisible() const;    // ShowTimeOnSeekBar() and there is a timer string to display
    int  TimeSectionEffectiveWidth() const; // px the section currently occupies (0 when not shown)
    void EnsureTimeFont() const;
    CString BuildTimeTemplate() const;  // widest time string for the currently enabled time options
    CRect GetTimeRect() const;          // the dedicated time section to the right of the channel

    virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
    virtual CSize CalcFixedLayout(BOOL bStretch, BOOL bHorz) override;

    void MoveThumb(const CPoint& point);
    void SeekToThumb(CPoint point, SeekMode mode);
    void checkHover(CPoint point);
    void invalidateThumb();
    long ChannelPointFromPosition(REFERENCE_TIME rtPos) const;
    REFERENCE_TIME PositionFromClientPoint(const CPoint& point) const;
    void SyncThumbToVideo(REFERENCE_TIME rtPos, REFERENCE_TIME rtPosDraw);

    void CreateThumb(bool bEnabled, CDC& parentDC);
    CRect GetChannelRowRect() const;    // the row the channel, time section and buttons share
    CRect GetBaseRect() const;          // full usable rect (channel + time section), before reserving the time area
    CRect GetChannelRect() const;
    CRect GetSeekableRect() const;      // client area that responds to seek/hover (excludes the time section)
    CRect GetThumbRect() const;
    CRect GetInnerThumbRect(bool bEnabled, const CRect& thumbRect) const;

    void UpdateTooltip(const CPoint& point);
    void UpdateToolTipText();
    void GenerateToolTipText(REFERENCE_TIME rtPos);

public:
    void Enable(bool bEnable);
    void HideToolTip();
    void UpdateToolTipPosition(CPoint point);

    void GetRange(REFERENCE_TIME& rtStart, REFERENCE_TIME& rtStop) const;
    void SetRange(REFERENCE_TIME rtStart, REFERENCE_TIME rtStop);
    REFERENCE_TIME GetPos() const;
    void SetPos(REFERENCE_TIME rtPos);
    bool HasDuration() const;
    REFERENCE_TIME GetDuration();

    void SetChapterBag(IDSMChapterBag* pCB);
    void RemoveChapters();

    bool DraggingThumb();
    void PreviewWindowShow(const CPoint point);
    void UpdateToolTipTextPreview(REFERENCE_TIME rtPos);

    void UpdateTime();

    void SetLosslessCutMode(bool bOn);  // shows the LosslessCut strip and grows the bar for it
    void OnLosslessCutChanged();        // segments or LosslessCut state changed: repaint
    void RelayoutAfterHeightChange(int heightBefore); // grows/shrinks the window so the video keeps its size

    // Zoom, smooth drag and exact drag (PlayerSeekBarTools.cpp); their buttons sit on the controls toolbar
    enum class SeekTool { Smooth, Exact, Zoom };
    bool IsZoomed() const;
    double ZoomFactor() const;
    void ZoomBy(double factor);         // around the playhead
    void ResetZoom();
    bool IsSmoothDrag() const;
    bool IsExactDrag() const;           // dragging and the hover preview ignore keyframes
    void SetSmoothStrength(int strength);
    void OnSeekToolClicked(SeekTool tool);
    void ShowSeekToolMenu(SeekTool tool, CPoint screenPt, CWnd* pOwner);
    CString SeekToolTip(SeekTool tool) const;

private:
    DECLARE_MESSAGE_MAP()

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnPaint();
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnXButtonDown(UINT nFlags, UINT nButton, CPoint point);
    afx_msg void OnXButtonUp(UINT nFlags, UINT nButton, CPoint point);
    afx_msg void OnXButtonDblClk(UINT nFlags, UINT nButton, CPoint point);
    afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnMouseLeave();
    afx_msg LRESULT OnThemeChanged();
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
    afx_msg BOOL OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnSmoothDrag();
    afx_msg void OnUpdateSmoothDrag(CCmdUI* pCmdUI);
    afx_msg void OnExactDrag();
    afx_msg void OnUpdateExactDrag(CCmdUI* pCmdUI);
    afx_msg void OnZoomIn();
    afx_msg void OnZoomOut();
    afx_msg void OnZoomReset();
    afx_msg void OnUpdateZoom(CCmdUI* pCmdUI);
};
