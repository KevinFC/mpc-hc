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

class CMainFrame;

// The smooth drag, exact drag and zoom buttons at the end of the controls toolbar's buttons.
// Custom drawn like the toolbar's own buttons; the seekbar does the actual work.
class CSeekBarToolButtons : public CWnd
{
public:
    explicit CSeekBarToolButtons(CMainFrame* pMainFrame);
    BOOL Create(CWnd* pParent);

    int GetGroupWidth(int buttonWidth) const;   // 0 when the buttons are turned off
    void Place(const CRect& rc, int buttonWidth);
    void SyncState();                           // repaints when what the buttons show has changed

private:
    static constexpr int kCount = 3;
    CMainFrame* m_pMainFrame;
    int m_buttonWidth = 0;
    int m_hot = -1, m_pressed = -1;
    bool m_bTracking = false;
    int m_wheelAccum = 0;
    unsigned m_state = ~0u;
    CMPCThemeToolTipCtrl m_tooltip;
    CString m_tipText;

    CRect ButtonRect(int i) const;
    int HitTest(CPoint pt) const;
    unsigned CurrentState() const;
    void UpdateTooltips();

    DECLARE_MESSAGE_MAP()
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnMouseLeave();
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg BOOL OnToolTipText(UINT id, NMHDR* pNMHDR, LRESULT* pResult);
};
