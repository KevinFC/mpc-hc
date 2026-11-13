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

#include "CMPCThemePlayerBar.h"
#include "CMPCThemeToolTipCtrl.h"

class CMainFrame;

// Inline editor for a segment name. Handles its own keys so the player's shortcuts
// (space, I, O, ...) don't fire while typing.
class CLosslessCutNameEdit : public CEdit
{
public:
    std::function<void(bool bCommit)> onDone;

protected:
    bool m_bDone = false;
    BOOL PreTranslateMessage(MSG* pMsg) override;
    DECLARE_MESSAGE_MAP()
    afx_msg UINT OnGetDlgCode();
    afx_msg void OnKillFocus(CWnd* pNewWnd);
    void Finish(bool bCommit);
};

// The segment cards shown in the LosslessCut panel. Custom drawn, like the strip under the seekbar.
class CLosslessCutSegmentList : public CWnd
{
public:
    explicit CLosslessCutSegmentList(CMainFrame* pMainFrame);
    BOOL Create(CWnd* pParent);
    void Refresh();

private:
    CMainFrame* m_pMainFrame;
    CFont m_font, m_fontBold, m_fontSmall;
    int m_fontDpi = 0;
    int m_scroll = 0;
    int m_hotRow = -1;
    bool m_bHotCheck = false;
    bool m_bTracking = false;
    bool m_bDraggingThumb = false;
    int m_dragStartY = 0, m_dragStartScroll = 0;
    int m_lastCurrent = -2;
    size_t m_lastCount = 0;
    int m_editRow = -1;
    CLosslessCutNameEdit m_edit;
    CBrush m_editBrush;
    CMPCThemeToolTipCtrl m_tooltip;
    CString m_tipText;

    void EnsureFonts();
    int HeaderHeight() const;
    int RowHeight() const;
    int ContentHeight() const;
    int MaxScroll() const;
    void ScrollTo(int y);
    void EnsureVisible(int row);
    CRect RowRect(int row) const;
    CRect CheckRect(const CRect& row) const;
    CRect NameRect(const CRect& row) const;
    bool ScrollbarRects(CRect& track, CRect& thumb) const;
    int RowAt(CPoint pt) const;
    void BeginRename(int row);
    void EndRename(bool bCommit);

    DECLARE_MESSAGE_MAP()
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnMouseLeave();
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg BOOL OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);
};

// Dockable panel listing the LosslessCut segments. Shown while LosslessCut mode is on and the
// user has it toggled on (CLosslessCutController::ShowSegmentList).
class CLosslessCutSegmentsBar : public CMPCThemePlayerBar
{
    DECLARE_DYNAMIC(CLosslessCutSegmentsBar)

public:
    explicit CLosslessCutSegmentsBar(CMainFrame* pMainFrame);
    BOOL Create(CWnd* pParentWnd, UINT defDockBarID);
    void Refresh();

    void ReloadTranslatableResources() override;
    void LoadState(CFrameWnd* pParent) override;

private:
    CLosslessCutSegmentList m_list;

    DECLARE_MESSAGE_MAP()
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnWindowPosChanged(WINDOWPOS* lpwndpos);
};
