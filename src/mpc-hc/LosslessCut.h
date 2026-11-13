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

#include "LosslessCutProject.h"
#include "CMPCThemeMenu.h"
#include <vector>

class CMainFrame;

// LosslessCut mode: shows and edits the cut segments of a LosslessCut project (<name>-proj.llc)
// next to the playing file, and can confine playback and seeking to those segments. Exporting
// stays in LosslessCut, which can be launched on the current file.
//
// Every edit is written to the project file immediately (see llc::FileIO::AtomicWrite). The
// file is watched, so changes saved by LosslessCut show up here; if both programs change the
// project, the version on disk wins and this side reloads instead of overwriting it.
//
// Composed into CMainFrame, which routes its commands here from OnCmdMsg.
class CLosslessCutController : public CCmdTarget
{
public:
    enum class Scope { Full = 0, Segments = 1, Single = 2 };
    enum class Status { Saved, NotSavedYet, Error, ReadOnly };

    struct Range {
        REFERENCE_TIME start;
        REFERENCE_TIME end;
    };

    explicit CLosslessCutController(CMainFrame& frame);

    static REFERENCE_TIME ToRT(double seconds);
    static double ToSeconds(REFERENCE_TIME rt);
    static CString FormatTime(REFERENCE_TIME rt, bool bAlwaysHours = false);

    bool IsActive() const { return m_bActive; }
    const std::vector<llc::Segment>& GetSegments() const { return m_project.segments; }
    int GetCurrentIndex() const { return m_iCurrent; }
    const llc::Segment* GetCurrentSegment() const;
    int SegmentAt(REFERENCE_TIME rt) const;   // -1 when no segment contains rt

    Scope GetScope() const;
    bool IsRestricted() const;
    bool IsLooping() const;
    bool CanEdit() const { return m_bActive && !m_bReadOnly; }
    bool CanUndo() const { return CanEdit() && !m_undo.empty(); }
    bool CanRedo() const { return CanEdit() && !m_redo.empty(); }
    bool HasKeyframes() const;
    bool IsOnKeyframe() const { return m_bOnKeyframe; }
    Status GetStatus() const;
    CString GetStatusText() const;
    CString GetProjectPath() const { return m_projectPath; }

    // The ranges playback and (when restricted) seeking are confined to. Empty means "no limits".
    std::vector<Range> GetActiveRanges() const;

    // --- hooks called by CMainFrame / the seekbar ---
    void OnMediaOpened();
    void OnMediaClosing(bool bNextIsQueued);
    void OnPositionUpdate(REFERENCE_TIME rtNow);
    bool EnforcePlayback(REFERENCE_TIME rtNow);        // true when it seeked or paused
    void OnBeforePlay();
    REFERENCE_TIME ConstrainSeek(REFERENCE_TIME rtTarget) const;
    REFERENCE_TIME ClampToRanges(REFERENCE_TIME rt) const;
    bool OffsetWithinRanges(REFERENCE_TIME rtFrom, REFERENCE_TIME rtDelta, REFERENCE_TIME& rtResult) const;
    bool WantsFastPolling() const;
    void OnWatchTimer();

    // In LosslessCut mode the plain I and O keys set the segment in and out points, like in
    // LosslessCut. Called from CMainFrame::PreTranslateMessage, before the accelerators.
    bool HandleKeyDown(const MSG* pMsg);

    // --- UI actions ---
    void SelectSegment(int index, bool bSeekToStart);
    void RenameSegment(int index, const CString& name);
    void ToggleSelected(int index);
    bool IsSegmentListShown() const;
    void ShowSegmentList(bool bShow);           // remembers the choice
    bool IsSyncingSegmentList() const { return m_bSyncingList; }
    void OnSegmentListShownOrHidden();          // keeps the strip's list button in sync
    void ShowContextMenu(CWnd* pOwner, CPoint screenPt, REFERENCE_TIME rtAt);
    void SetActive(bool bActive);

private:
    CMainFrame& m_frame;

    struct Snapshot {
        std::vector<llc::Segment> segments;
        int current;
    };

    bool m_bActive = false;
    bool m_bUserDisabled = false;       // turned off by hand: don't switch back on by itself
    bool m_bSyncingList = false;        // showing/hiding the segment list on our own, not by the user
    bool m_bReadOnly = false;
    CString m_readOnlyReason;
    CString m_mediaPath;
    CString m_projectPath;
    llc::Project m_project;
    int m_iCurrent = -1;
    std::vector<Snapshot> m_undo, m_redo;

    // What this side last read from or wrote to the project file.
    bool m_bFileKnown = false;          // m_knownBytes reflects the file on disk
    std::string m_knownBytes;
    llc::FileIO::Stamp m_knownStamp;
    bool m_bDirty = false;              // in-memory segments not yet on disk
    CString m_lastError;
    bool m_bBackedUp = false;
    int m_externalParseFailures = 0;

    bool m_bOnKeyframe = false;
    REFERENCE_TIME m_rtLastPos = 0;
    ULONGLONG m_lastEnforceTick = 0;
    REFERENCE_TIME m_rtLastEnforceTarget = -1;
    REFERENCE_TIME m_rtAddAt = -1;      // position for "Add segment" from the context menu

    CMPCThemeMenu m_menu;

    REFERENCE_TIME FrameDuration() const;
    REFERENCE_TIME Duration() const;
    REFERENCE_TIME Position() const;
    bool IsRunning() const;

    bool LoadFromDisk(bool bInitial);
    bool ApplyDiskBytes(const std::string& bytes, CString& error);
    void Commit(std::vector<llc::Segment> segments, int current, const CString& osd);
    bool Save();
    CString BackupFolder() const;
    void NotifyChanged(bool bLayout = false);
    void SyncSegmentList();                     // shown while LosslessCut mode is on and the user wants it
    void Osd(const CString& text, int ms = 2000) const;
    void SetScope(Scope scope);
    void SeekTo(REFERENCE_TIME rt);
    int InsertSorted(std::vector<llc::Segment>& segs, llc::Segment seg) const;
    bool LocateLosslessCut(CString& exePath);

    void SetStart(REFERENCE_TIME rt);
    void SetEnd(REFERENCE_TIME rt);
    void Split(REFERENCE_TIME rt);

    DECLARE_MESSAGE_MAP()

    afx_msg void OnToggleMode();
    afx_msg void OnUpdateToggleMode(CCmdUI* pCmdUI);
    afx_msg void OnSetStart();
    afx_msg void OnSetEnd();
    afx_msg void OnAddSegment();
    afx_msg void OnSplitSegment();
    afx_msg void OnRemoveSegment();
    afx_msg void OnToggleSelected();
    afx_msg void OnUpdateEdit(CCmdUI* pCmdUI);
    afx_msg void OnPrevSegment();
    afx_msg void OnNextSegment();
    afx_msg void OnUpdateNavigate(CCmdUI* pCmdUI);
    afx_msg void OnUndo();
    afx_msg void OnRedo();
    afx_msg void OnUpdateUndo(CCmdUI* pCmdUI);
    afx_msg void OnUpdateRedo(CCmdUI* pCmdUI);
    afx_msg void OnScope(UINT nID);
    afx_msg void OnUpdateScope(CCmdUI* pCmdUI);
    afx_msg void OnToggleRestrict();
    afx_msg void OnToggleLoop();
    afx_msg void OnUpdateToggles(CCmdUI* pCmdUI);
    afx_msg void OnLaunch();
    afx_msg void OnUpdateLaunch(CCmdUI* pCmdUI);
    afx_msg void OnToggleSegmentList();
    afx_msg void OnUpdateToggleSegmentList(CCmdUI* pCmdUI);
};
