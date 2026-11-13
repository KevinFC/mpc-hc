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
#include "LosslessCut.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "PathUtils.h"
#include "FullscreenWnd.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr REFERENCE_TIME ONE_SECOND = 10000000LL;
    constexpr REFERENCE_TIME DEFAULT_NEW_SEGMENT_LENGTH = 10 * ONE_SECOND; // same as LosslessCut
    constexpr size_t MAX_UNDO = 200;
    constexpr int MAX_BACKUPS_PER_PROJECT = 20;
    constexpr UINT WATCH_INTERVAL_MS = 1000;

    CString W(const std::string& utf8)
    {
        return CString(llc::Utf8ToWide(utf8).c_str());
    }
}

CLosslessCutController::CLosslessCutController(CMainFrame& frame)
    : m_frame(frame)
{
}

// -------------------------------------------------------------------------------------------------
// Helpers

REFERENCE_TIME CLosslessCutController::ToRT(double seconds)
{
    return static_cast<REFERENCE_TIME>(std::llround(seconds * 10000000.0));
}

double CLosslessCutController::ToSeconds(REFERENCE_TIME rt)
{
    return rt / 10000000.0;
}

CString CLosslessCutController::FormatTime(REFERENCE_TIME rt, bool bAlwaysHours)
{
    if (rt < 0) {
        rt = 0;
    }
    const LONGLONG ms = (rt + 5000) / 10000;
    const LONGLONG h = ms / 3600000, m = (ms / 60000) % 60, sec = (ms / 1000) % 60, frac = ms % 1000;
    CString str;
    if (h > 0 || bAlwaysHours) {
        str.Format(_T("%02lld:%02lld:%02lld.%03lld"), h, m, sec, frac);
    } else {
        str.Format(_T("%02lld:%02lld.%03lld"), m, sec, frac);
    }
    return str;
}

REFERENCE_TIME CLosslessCutController::FrameDuration() const
{
    const REFTIME frame = m_frame.GetAvgTimePerFrame();
    if (frame > 0.0 && frame < 1.0) {
        return static_cast<REFERENCE_TIME>(frame * 10000000.0);
    }
    return 400000; // 40 ms
}

REFERENCE_TIME CLosslessCutController::Duration() const
{
    return m_frame.GetDur();
}

REFERENCE_TIME CLosslessCutController::Position() const
{
    // Ask the graph rather than the seekbar: right after a frame step or keyframe jump the
    // seekbar can be one poll behind, and edits must land on the frame that is on screen.
    if (m_frame.GetLoadState() == MLS::LOADED && m_frame.m_graph.m_pMS) {
        REFERENCE_TIME rt = 0;
        if (SUCCEEDED(m_frame.m_graph.m_pMS->GetCurrentPosition(&rt))) {
            return rt;
        }
    }
    return m_frame.GetPos();
}

bool CLosslessCutController::IsRunning() const
{
    return m_frame.GetMediaState() == State_Running;
}

void CLosslessCutController::Osd(const CString& text, int ms) const
{
    if (!text.IsEmpty()) {
        m_frame.m_OSD.DisplayMessage(OSD_TOPLEFT, text, ms);
    }
}

void CLosslessCutController::NotifyChanged(bool bLayout)
{
    if (bLayout) {
        auto& bar = static_cast<CControlBar&>(m_frame.m_wndSeekBar);
        const int before = bar.CalcFixedLayout(TRUE, TRUE).cy;
        m_frame.m_wndSeekBar.SetLosslessCutMode(m_bActive);
        const int after = bar.CalcFixedLayout(TRUE, TRUE).cy;
        if (after != before && m_frame.m_wndSeekBar.IsWindowVisible() && !m_frame.IsZoomed() && !m_frame.IsIconic() &&
                !m_frame.IsFullScreenMode() && !m_frame.m_controls.ToolbarsCoverVideo()) {
            // Grow/shrink the window like toggling a toolbar does, so the video keeps its size.
            CRect r;
            m_frame.GetWindowRect(r);
            r.bottom += after - before;
            m_frame.MoveWindow(r);
        }
        m_frame.RecalcLayout();
        SyncSegmentList();
    }
    m_frame.m_wndSeekBar.OnLosslessCutChanged();
    m_frame.m_wndLlcSegmentsBar.Refresh();
}

bool CLosslessCutController::IsSegmentListShown() const
{
    return m_frame.m_controls.ControlChecked(CMainFrameControls::Panel::LOSSLESSCUT);
}

void CLosslessCutController::SyncSegmentList()
{
    const bool want = m_bActive && AfxGetAppSettings().bLosslessCutShowList;
    if (want != IsSegmentListShown()) {
        m_bSyncingList = true;
        m_frame.m_controls.ToggleControl(CMainFrameControls::Panel::LOSSLESSCUT);
        m_bSyncingList = false;
    }
}

void CLosslessCutController::OnSegmentListShownOrHidden()
{
    m_frame.m_wndSeekBar.OnLosslessCutChanged();
}

void CLosslessCutController::ShowSegmentList(bool bShow)
{
    AfxGetAppSettings().bLosslessCutShowList = bShow;
    SyncSegmentList();
    NotifyChanged();
}

bool CLosslessCutController::HandleKeyDown(const MSG* pMsg)
{
    if (!m_bActive || pMsg->message != WM_KEYDOWN || (pMsg->wParam != 'I' && pMsg->wParam != 'O')) {
        return false;
    }
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
        return false; // modified keys keep their normal bindings
    }
    // Only when the player itself has the keyboard, never while typing in an edit box or a dialog.
    if (HWND focus = ::GetFocus()) {
        TCHAR cls[64] = {};
        ::GetClassName(focus, cls, _countof(cls));
        if (_tcsicmp(cls, _T("Edit")) == 0 || _tcsnicmp(cls, _T("RichEdit"), 8) == 0 || _tcsicmp(cls, _T("ComboBox")) == 0 ||
                _tcsicmp(cls, _T("msctls_hotkey32")) == 0) {
            return false;
        }
        const HWND root = ::GetAncestor(focus, GA_ROOT);
        const HWND fsWnd = m_frame.m_pDedicatedFSVideoWnd ? m_frame.m_pDedicatedFSVideoWnd->GetSafeHwnd() : nullptr;
        if (root != m_frame.GetSafeHwnd() && (!fsWnd || root != fsWnd)) {
            return false;
        }
    }
    if (!(pMsg->lParam & (1 << 30))) { // ignore auto-repeat while the key is held
        m_frame.SendMessage(WM_COMMAND, pMsg->wParam == 'I' ? ID_LLC_SET_START : ID_LLC_SET_END);
    }
    return true;
}

const llc::Segment* CLosslessCutController::GetCurrentSegment() const
{
    const auto& segs = m_project.segments;
    return (m_iCurrent >= 0 && m_iCurrent < static_cast<int>(segs.size())) ? &segs[m_iCurrent] : nullptr;
}

int CLosslessCutController::SegmentAt(REFERENCE_TIME rt) const
{
    const auto& segs = m_project.segments;
    const REFERENCE_TIME tol = FrameDuration() / 2;
    int best = -1;
    for (int i = 0; i < static_cast<int>(segs.size()); i++) {
        if (segs[i].IsMarker()) {
            continue;
        }
        const REFERENCE_TIME s = ToRT(segs[i].start), e = ToRT(*segs[i].end);
        if (rt + tol >= s && rt + tol < e && (best < 0 || segs[i].start > segs[best].start)) {
            best = i;
        }
    }
    return best;
}

int CLosslessCutController::InsertSorted(std::vector<llc::Segment>& segs, llc::Segment seg) const
{
    auto it = std::find_if(segs.begin(), segs.end(), [&](const llc::Segment& s) { return s.start > seg.start; });
    const int index = static_cast<int>(it - segs.begin());
    segs.insert(it, std::move(seg));
    return index;
}

CLosslessCutController::Scope CLosslessCutController::GetScope() const
{
    const int v = AfxGetAppSettings().iLosslessCutScope;
    return (v == 1) ? Scope::Segments : (v == 2) ? Scope::Single : Scope::Full;
}

bool CLosslessCutController::IsRestricted() const
{
    return AfxGetAppSettings().bLosslessCutRestrict;
}

bool CLosslessCutController::IsLooping() const
{
    return AfxGetAppSettings().bLosslessCutLoop;
}

bool CLosslessCutController::HasKeyframes() const
{
    return !m_frame.m_kfs.empty();
}

CLosslessCutController::Status CLosslessCutController::GetStatus() const
{
    if (m_bReadOnly) {
        return Status::ReadOnly;
    }
    if (m_bDirty) {
        return Status::Error;
    }
    return m_bFileKnown ? Status::Saved : Status::NotSavedYet;
}

CString CLosslessCutController::GetStatusText() const
{
    const CString name = PathFindFileName(m_projectPath);
    switch (GetStatus()) {
        case Status::ReadOnly:
            return _T("Read-only: ") + m_readOnlyReason + _T("\nChanges are not saved, so the project file stays untouched.");
        case Status::Error:
            return _T("Not saved: ") + m_lastError + _T("\nRetrying automatically. Your changes are kept.");
        case Status::NotSavedYet:
            return _T("No project file yet.\n") + name + _T(" is created next to the video on the first change.");
        case Status::Saved:
        default:
            return _T("All changes saved to ") + name;
    }
}

std::vector<CLosslessCutController::Range> CLosslessCutController::GetActiveRanges() const
{
    std::vector<Range> ranges;
    if (!m_bActive) {
        return ranges;
    }
    const REFERENCE_TIME dur = Duration();
    auto add = [&](const llc::Segment& seg) {
        if (seg.IsMarker()) {
            return;
        }
        REFERENCE_TIME s = std::max(0LL, ToRT(seg.start)), e = ToRT(*seg.end);
        if (dur > 0) {
            e = std::min(e, dur);
        }
        if (e > s) {
            ranges.push_back({ s, e });
        }
    };

    switch (GetScope()) {
        case Scope::Full:
            break;
        case Scope::Single:
            if (const llc::Segment* seg = GetCurrentSegment()) {
                add(*seg);
            }
            break;
        case Scope::Segments: {
            for (const auto& seg : m_project.segments) {
                if (seg.selected) {
                    add(seg);
                }
            }
            std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
            std::vector<Range> merged;
            for (const Range& r : ranges) {
                if (!merged.empty() && r.start <= merged.back().end) {
                    merged.back().end = std::max(merged.back().end, r.end);
                } else {
                    merged.push_back(r);
                }
            }
            ranges.swap(merged);
            break;
        }
    }
    return ranges;
}

// -------------------------------------------------------------------------------------------------
// Playback scope and seek restriction

bool CLosslessCutController::WantsFastPolling() const
{
    return m_bActive && GetScope() != Scope::Full;
}

bool CLosslessCutController::EnforcePlayback(REFERENCE_TIME rtNow)
{
    if (!m_bActive || GetScope() == Scope::Full || !IsRunning()) {
        return false;
    }
    const auto ranges = GetActiveRanges();
    if (ranges.empty()) {
        return false;
    }
    const REFERENCE_TIME tol = FrameDuration() / 2;

    REFERENCE_TIME target = -1;
    for (const Range& r : ranges) {
        if (rtNow + tol < r.end) {
            if (rtNow + tol >= r.start) {
                return false; // inside, nothing to do
            }
            target = r.start; // in a gap: skip ahead to the next range
            break;
        }
    }

    if (target < 0) {
        if (IsLooping()) {
            target = ranges.front().start;
        } else {
            // Past the last range: stop on its last frame. Play starts over (OnBeforePlay).
            m_frame.SendMessage(WM_COMMAND, ID_PLAY_PAUSE);
            m_frame.DoSeekTo(std::max(ranges.back().start, ranges.back().end - FrameDuration()), false);
            return true;
        }
    }

    // Don't hammer the graph if a seek does not get there (e.g. a non-seekable stream).
    const ULONGLONG now = GetTickCount64();
    if (target == m_rtLastEnforceTarget && now - m_lastEnforceTick < 250) {
        return false;
    }
    m_rtLastEnforceTarget = target;
    m_lastEnforceTick = now;
    m_frame.DoSeekTo(target, false);
    return true;
}

void CLosslessCutController::OnBeforePlay()
{
    // From stopped the position is reset anyway; the first poll moves it into the first range.
    if (!m_bActive || GetScope() == Scope::Full || m_frame.GetMediaState() != State_Paused) {
        return;
    }
    const auto ranges = GetActiveRanges();
    if (ranges.empty()) {
        return;
    }
    const REFERENCE_TIME pos = Position();
    const REFERENCE_TIME frame = FrameDuration();
    for (const Range& r : ranges) {
        if (pos + frame + frame / 2 < r.end) {
            if (pos + frame / 2 < r.start) {
                m_frame.DoSeekTo(r.start, false);
            }
            return;
        }
    }
    m_frame.DoSeekTo(ranges.front().start, false); // at the end: play from the beginning
}

REFERENCE_TIME CLosslessCutController::ConstrainSeek(REFERENCE_TIME rtTarget) const
{
    if (!m_bActive || !IsRestricted() || GetScope() == Scope::Full) {
        return rtTarget;
    }
    const auto ranges = GetActiveRanges();
    if (ranges.empty()) {
        return rtTarget;
    }
    const REFERENCE_TIME frame = FrameDuration();
    auto lastFrame = [frame](const Range& r) { return std::max(r.start, r.end - frame); };

    for (const Range& r : ranges) {
        if (rtTarget >= r.start && rtTarget <= lastFrame(r)) {
            return rtTarget;
        }
    }
    if (rtTarget > m_frame.GetPos()) {
        for (const Range& r : ranges) {
            if (r.start > rtTarget) {
                return r.start;
            }
        }
        return lastFrame(ranges.back());
    }
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        if (lastFrame(*it) < rtTarget) {
            return lastFrame(*it);
        }
    }
    return ranges.front().start;
}

REFERENCE_TIME CLosslessCutController::ClampToRanges(REFERENCE_TIME rt) const
{
    if (!m_bActive || !IsRestricted() || GetScope() == Scope::Full) {
        return rt;
    }
    const auto ranges = GetActiveRanges();
    if (ranges.empty()) {
        return rt;
    }
    const REFERENCE_TIME frame = FrameDuration();
    REFERENCE_TIME best = rt, bestDist = LLONG_MAX;
    for (const Range& r : ranges) {
        const REFERENCE_TIME last = std::max(r.start, r.end - frame);
        if (rt >= r.start && rt <= last) {
            return rt;
        }
        for (REFERENCE_TIME edge : { r.start, last }) {
            const REFERENCE_TIME d = std::abs(edge - rt);
            if (d < bestDist) {
                bestDist = d;
                best = edge;
            }
        }
    }
    return best;
}

bool CLosslessCutController::OffsetWithinRanges(REFERENCE_TIME rtFrom, REFERENCE_TIME rtDelta, REFERENCE_TIME& rtResult) const
{
    if (!m_bActive || !IsRestricted() || GetScope() == Scope::Full) {
        return false;
    }
    const auto ranges = GetActiveRanges();
    if (ranges.empty()) {
        return false;
    }
    // Treat the ranges as one continuous timeline, so a 5 second jump near the end of a
    // segment continues into the next one instead of stopping at the gap.
    REFERENCE_TIME total = 0, virt = 0;
    bool located = false;
    for (const Range& r : ranges) {
        const REFERENCE_TIME len = r.end - r.start;
        if (!located) {
            if (rtFrom >= r.end) {
                virt += len;
            } else {
                virt += std::max(0LL, rtFrom - r.start);
                located = true;
            }
        }
        total += len;
    }
    const REFERENCE_TIME target = std::clamp(virt + rtDelta, 0LL, total);
    REFERENCE_TIME acc = 0;
    for (const Range& r : ranges) {
        const REFERENCE_TIME len = r.end - r.start;
        if (target < acc + len) {
            rtResult = r.start + (target - acc);
            return true;
        }
        acc += len;
    }
    rtResult = std::max(ranges.back().start, ranges.back().end - FrameDuration());
    return true;
}

void CLosslessCutController::OnPositionUpdate(REFERENCE_TIME rtNow)
{
    if (!m_bActive) {
        return;
    }
    bool changed = false;

    bool onKeyframe = false;
    const auto& kfs = m_frame.m_kfs;
    if (!kfs.empty()) {
        const REFERENCE_TIME tol = std::max(FrameDuration() / 2, 10000LL);
        auto it = std::lower_bound(kfs.begin(), kfs.end(), rtNow - tol);
        onKeyframe = (it != kfs.end() && *it <= rtNow + tol);
    }
    if (onKeyframe != m_bOnKeyframe) {
        m_bOnKeyframe = onKeyframe;
        changed = true;
    }

    // The current segment follows the playhead, except in single-segment mode where it is the
    // range being played.
    if (GetScope() != Scope::Single) {
        const int idx = SegmentAt(rtNow);
        if (idx >= 0 && idx != m_iCurrent) {
            m_iCurrent = idx;
            changed = true;
        }
    }
    if (changed) {
        NotifyChanged();
    }
}

void CLosslessCutController::SeekTo(REFERENCE_TIME rt)
{
    if (m_frame.GetLoadState() == MLS::LOADED) {
        m_frame.SeekTo(rt);
    }
}

void CLosslessCutController::SetScope(Scope scope)
{
    auto& s = AfxGetAppSettings();
    s.iLosslessCutScope = static_cast<int>(scope);
    if (scope == Scope::Full) {
        Osd(_T("Playing everything"));
    } else if (scope == Scope::Segments) {
        Osd(_T("Playing segments only"));
    } else {
        Osd(_T("Playing the current segment only"));
    }
    m_frame.AdjustStreamPosPoller(true);
    if (scope != Scope::Full && IsRestricted() && m_frame.GetLoadState() == MLS::LOADED) {
        const REFERENCE_TIME pos = m_frame.GetPos();
        const REFERENCE_TIME clamped = ClampToRanges(pos);
        if (clamped != pos) {
            m_frame.DoSeekTo(clamped, false);
        }
    }
    NotifyChanged();
}

// -------------------------------------------------------------------------------------------------
// Project file

CString CLosslessCutController::BackupFolder() const
{
    CString path;
    if (AfxGetMyApp()->GetAppDataPath(path)) {
        return PathUtils::CombinePaths(path, _T("LosslessCut backups"));
    }
    return PathUtils::CombinePaths(PathUtils::GetProgramPath(), _T("LosslessCut backups"));
}

bool CLosslessCutController::ApplyDiskBytes(const std::string& bytes, CString& error)
{
    llc::Project project;
    std::string err;
    const auto status = project.Load(bytes, err);
    if (status != llc::Project::LoadStatus::Ok) {
        error = (status == llc::Project::LoadStatus::UnsupportedVersion)
                ? CString(_T("it was saved by a newer LosslessCut version"))
                : _T("it could not be read (") + W(err) + _T(")");
        return false;
    }
    m_project = std::move(project);
    m_knownBytes = bytes;
    m_knownStamp = llc::FileIO::GetStamp(std::wstring(m_projectPath));
    m_bFileKnown = true;
    m_bDirty = false;
    m_bReadOnly = false;
    m_readOnlyReason.Empty();
    m_lastError.Empty();
    m_externalParseFailures = 0;
    m_undo.clear();
    m_redo.clear();
    if (m_iCurrent >= static_cast<int>(m_project.segments.size())) {
        m_iCurrent = static_cast<int>(m_project.segments.size()) - 1;
    }
    return true;
}

bool CLosslessCutController::LoadFromDisk(bool bInitial)
{
    UNREFERENCED_PARAMETER(bInitial);
    const std::wstring path(m_projectPath);
    m_bDirty = false;
    m_lastError.Empty();
    m_undo.clear();
    m_redo.clear();
    m_iCurrent = -1;
    m_bReadOnly = false;
    m_readOnlyReason.Empty();
    m_externalParseFailures = 0;

    const auto stamp = llc::FileIO::GetStamp(path);
    if (!stamp.exists) {
        m_project.InitNew(std::wstring(m_mediaPath));
        m_bFileKnown = false;
        m_knownBytes.clear();
        m_knownStamp = stamp;
        return true;
    }

    std::string bytes;
    DWORD winErr;
    if (!llc::FileIO::ReadAll(path, bytes, winErr)) {
        m_project.InitNew(std::wstring(m_mediaPath));
        m_bReadOnly = true;
        m_readOnlyReason.Format(_T("the project file could not be opened (error %lu)"), winErr);
        m_bFileKnown = false;
        m_knownStamp = stamp;
        return false;
    }

    CString error;
    if (!ApplyDiskBytes(bytes, error)) {
        m_project.InitNew(std::wstring(m_mediaPath));
        m_bReadOnly = true;
        m_readOnlyReason = error;
        m_knownBytes = bytes;
        m_knownStamp = stamp;
        m_bFileKnown = true;
        return false;
    }
    if (!m_project.segments.empty()) {
        m_iCurrent = SegmentAt(m_frame.GetPos());
        if (m_iCurrent < 0) {
            m_iCurrent = 0;
        }
    }
    return true;
}

bool CLosslessCutController::Save()
{
    if (!m_bDirty) {
        return true;
    }
    if (m_bReadOnly || m_projectPath.IsEmpty()) {
        return false;
    }
    const std::wstring path(m_projectPath);

    // 1. Never overwrite a version this side hasn't seen: if LosslessCut saved in the meantime,
    //    its version wins and is loaded instead.
    const auto stamp = llc::FileIO::GetStamp(path);
    const bool stampChanged = m_bFileKnown ? (stamp != m_knownStamp) : stamp.exists;
    if (stampChanged) {
        if (!stamp.exists) {
            m_bFileKnown = false; // removed outside the player: write it again below
            m_knownBytes.clear();
        } else {
            std::string disk;
            DWORD winErr;
            if (!llc::FileIO::ReadAll(path, disk, winErr)) {
                m_lastError.Format(_T("the project file is busy (error %lu)"), winErr);
                return false; // stays dirty, retried by the watch timer
            }
            if (m_bFileKnown && disk == m_knownBytes) {
                m_knownStamp = stamp; // only touched
            } else {
                CString error;
                if (ApplyDiskBytes(disk, error)) {
                    Osd(_T("LosslessCut changed the project, so it was reloaded. Your last change was not applied."), 5000);
                    NotifyChanged();
                } else {
                    // Possibly a half-written file (LosslessCut does not write atomically);
                    // keep the change in memory and look again shortly.
                    m_lastError = _T("the project file was changed by another program and ") + error;
                }
                return false;
            }
        }
    }

    // 2. One backup per session of the version found on disk, before it is first replaced.
    if (!m_bBackedUp && stamp.exists) {
        std::wstring error;
        if (!llc::FileIO::Backup(path, std::wstring(BackupFolder()), MAX_BACKUPS_PER_PROJECT, error)) {
            Osd(_T("Could not back up the LosslessCut project: ") + CString(error.c_str()), 4000);
        }
        m_bBackedUp = true;
    }

    // 3. Render and check the result before it goes anywhere near the disk.
    const std::string bytes = m_project.Serialize();
    std::string verifyError;
    if (!m_project.VerifySerialized(bytes, verifyError)) {
        m_lastError = W(verifyError);
        Osd(_T("LosslessCut project not saved: ") + m_lastError, 5000);
        return false;
    }

    // 4. Atomic replace, read back and compared.
    std::wstring writeError;
    if (!llc::FileIO::AtomicWrite(path, bytes, writeError)) {
        m_lastError = writeError.c_str();
        Osd(_T("LosslessCut project not saved: ") + m_lastError, 5000);
        return false;
    }
    m_knownBytes = bytes;
    m_knownStamp = llc::FileIO::GetStamp(path);
    m_bFileKnown = true;
    m_bDirty = false;
    m_lastError.Empty();
    return true;
}

void CLosslessCutController::Commit(std::vector<llc::Segment> segments, int current, const CString& osd)
{
    if (!m_bActive) {
        return;
    }
    if (m_bReadOnly) {
        Osd(_T("The LosslessCut project is read-only: ") + m_readOnlyReason, 4000);
        return;
    }
    m_undo.push_back({ m_project.segments, m_iCurrent });
    if (m_undo.size() > MAX_UNDO) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_project.segments = std::move(segments);
    m_iCurrent = current;
    m_bDirty = true;
    const bool saved = Save();
    NotifyChanged();
    if (saved) {
        Osd(osd);
    }
}

void CLosslessCutController::OnWatchTimer()
{
    if (m_mediaPath.IsEmpty()) {
        return;
    }
    const std::wstring path(m_projectPath);
    const auto stamp = llc::FileIO::GetStamp(path);

    if (!m_bActive) {
        // LosslessCut may create the project while the file is playing here.
        if (stamp.exists && !m_bUserDisabled && AfxGetAppSettings().bLosslessCutAutoEnable) {
            SetActive(true);
            Osd(_T("LosslessCut project found"));
        }
        return;
    }

    if (m_bDirty && !m_bReadOnly) {
        const bool wasError = !m_lastError.IsEmpty();
        if (Save() && wasError) {
            Osd(_T("LosslessCut project saved"));
        }
        NotifyChanged();
        return;
    }

    if (stamp == m_knownStamp) {
        return;
    }
    if (!stamp.exists) {
        if (m_bFileKnown) {
            m_bFileKnown = false;
            m_knownBytes.clear();
            m_knownStamp = stamp;
            Osd(_T("The LosslessCut project file was removed. The segments stay here and are saved again on the next change."), 4000);
            NotifyChanged();
        }
        return;
    }

    std::string bytes;
    DWORD winErr;
    if (!llc::FileIO::ReadAll(path, bytes, winErr)) {
        return; // busy, try again next tick
    }
    if (m_bFileKnown && bytes == m_knownBytes) {
        m_knownStamp = stamp;
        return;
    }
    CString error;
    if (ApplyDiskBytes(bytes, error)) {
        Osd(_T("Segments reloaded (changed in LosslessCut)"));
        NotifyChanged();
    } else if (++m_externalParseFailures >= 3) {
        // Not a half-written file: keep showing the last good segments, but stop writing.
        m_bReadOnly = true;
        m_readOnlyReason = error;
        m_knownBytes = bytes;
        m_knownStamp = stamp;
        m_bFileKnown = true;
        m_externalParseFailures = 0;
        Osd(_T("The LosslessCut project is now read-only: ") + error, 5000);
        NotifyChanged();
    }
}

// -------------------------------------------------------------------------------------------------
// Activation

void CLosslessCutController::OnMediaOpened()
{
    m_mediaPath.Empty();
    m_projectPath.Empty();
    m_bUserDisabled = false;
    m_bBackedUp = false;
    m_bOnKeyframe = false;
    m_rtLastEnforceTarget = -1;

    const CString path = m_frame.lastOpenFile;
    if (m_frame.GetPlaybackMode() != PM_FILE || path.IsEmpty() || PathIsURL(path) || !PathFileExists(path)) {
        m_bActive = false;
        NotifyChanged(true);
        return;
    }
    m_mediaPath = path;
    m_projectPath = llc::Project::ProjectPathForMedia(std::wstring(path)).c_str();
    m_frame.SetTimer(CMainFrame::TIMER_LLC_WATCH, WATCH_INTERVAL_MS, nullptr);

    // On by itself only when a project exists (and auto-enable is on). Turned on from the View menu,
    // it lasts for this file only; without a project the file is created by the first change.
    const bool exists = llc::FileIO::GetStamp(std::wstring(m_projectPath)).exists;
    const bool activate = exists && AfxGetAppSettings().bLosslessCutAutoEnable;
    if (activate) {
        LoadFromDisk(true);
        if (m_bReadOnly) {
            Osd(_T("LosslessCut project opened read-only: ") + m_readOnlyReason, 5000);
        }
    }
    m_bActive = activate;
    NotifyChanged(true); // compares against the seekbar's current layout, so it is cheap when nothing changes
    m_frame.AdjustStreamPosPoller(true);
}

void CLosslessCutController::OnMediaClosing(bool bNextIsQueued)
{
    m_frame.KillTimer(CMainFrame::TIMER_LLC_WATCH);
    if (m_bActive && m_bDirty) {
        while (!Save() && m_bDirty && !m_bReadOnly) {
            CString msg;
            msg.Format(_T("Your latest segment changes could not be saved to\n%s\n\n%s\n\nRetry?"),
                       m_projectPath.GetString(), m_lastError.GetString());
            if (AfxMessageBox(msg, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
                // Keep the work: drop a recovery copy next to the backups.
                const std::string bytes = m_project.Serialize();
                SYSTEMTIME st;
                GetLocalTime(&st);
                CString name;
                name.Format(_T("%s.recovered-%04u%02u%02u-%02u%02u%02u.llc"), PathFindFileName(m_projectPath),
                            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                const CString folder = BackupFolder();
                SHCreateDirectoryEx(nullptr, folder, nullptr);
                const CString recovery = PathUtils::CombinePaths(folder, name);
                std::wstring error;
                if (llc::FileIO::AtomicWrite(std::wstring(recovery), bytes, error)) {
                    AfxMessageBox(_T("The segments were saved to\n") + recovery, MB_ICONINFORMATION);
                } else {
                    AfxMessageBox(_T("The segments could not be saved anywhere: ") + CString(error.c_str()), MB_ICONERROR);
                }
                break;
            }
        }
    }
    m_mediaPath.Empty();
    m_bDirty = false;
    m_project.segments.clear();
    m_iCurrent = -1;
    m_undo.clear();
    m_redo.clear();
    if (m_bActive) {
        m_bActive = false;
        m_frame.AdjustStreamPosPoller(true);
        // When the next file follows right away, keep the layout; OnMediaOpened settles it,
        // which avoids the window shrinking and growing again between files.
        NotifyChanged(!bNextIsQueued);
    }
}

void CLosslessCutController::SetActive(bool bActive)
{
    if (bActive == m_bActive) {
        return;
    }
    if (bActive) {
        if (m_mediaPath.IsEmpty()) {
            Osd(_T("LosslessCut mode needs a local video file"));
            return;
        }
        LoadFromDisk(false);
        m_bActive = true;
        m_bUserDisabled = false;
        if (m_bReadOnly) {
            Osd(_T("LosslessCut project opened read-only: ") + m_readOnlyReason, 5000);
        }
    } else {
        if (m_bDirty) {
            Save();
        }
        m_bActive = false;
        m_bUserDisabled = true;
    }
    m_frame.AdjustStreamPosPoller(true);
    NotifyChanged(true);
}

// -------------------------------------------------------------------------------------------------
// Editing

void CLosslessCutController::SelectSegment(int index, bool bSeekToStart)
{
    const auto& segs = m_project.segments;
    if (!m_bActive || index < 0 || index >= static_cast<int>(segs.size())) {
        return;
    }
    m_iCurrent = index;
    NotifyChanged();
    if (bSeekToStart) {
        SeekTo(ToRT(segs[index].start));
    }
}

void CLosslessCutController::SetStart(REFERENCE_TIME rt)
{
    auto segs = m_project.segments;
    CString osd;
    if (m_iCurrent >= 0 && m_iCurrent < static_cast<int>(segs.size())) {
        auto& seg = segs[m_iCurrent];
        if (seg.IsMarker() || rt < ToRT(*seg.end)) {
            seg.start = ToSeconds(rt);
            osd.Format(_T("Segment %d start: %s"), m_iCurrent + 1, FormatTime(rt).GetString());
            Commit(std::move(segs), m_iCurrent, osd);
            return;
        }
    }
    // Past the end of the current segment: begin a new one here. It stays a marker until its end is set.
    llc::Segment seg;
    seg.start = ToSeconds(rt);
    const int idx = InsertSorted(segs, std::move(seg));
    osd.Format(_T("Segment %d starts at %s, set its end next"), idx + 1, FormatTime(rt).GetString());
    Commit(std::move(segs), idx, osd);
}

void CLosslessCutController::SetEnd(REFERENCE_TIME rt)
{
    auto segs = m_project.segments;
    CString osd;
    if (m_iCurrent >= 0 && m_iCurrent < static_cast<int>(segs.size()) && rt > ToRT(segs[m_iCurrent].start)) {
        segs[m_iCurrent].end = ToSeconds(rt);
        osd.Format(_T("Segment %d end: %s"), m_iCurrent + 1, FormatTime(rt).GetString());
        Commit(std::move(segs), m_iCurrent, osd);
        return;
    }
    // No segment to end here: create one from the previous cut point (or the start of the file).
    REFERENCE_TIME from = 0;
    for (const auto& s : segs) {
        const REFERENCE_TIME edge = ToRT(s.end ? *s.end : s.start);
        if (edge <= rt && edge > from) {
            from = edge;
        }
    }
    if (rt - from < FrameDuration()) {
        Osd(_T("Move past the segment start before setting its end"));
        return;
    }
    llc::Segment seg;
    seg.start = ToSeconds(from);
    seg.end = ToSeconds(rt);
    const int idx = InsertSorted(segs, std::move(seg));
    osd.Format(_T("Segment %d: %s - %s"), idx + 1, FormatTime(from).GetString(), FormatTime(rt).GetString());
    Commit(std::move(segs), idx, osd);
}

void CLosslessCutController::Split(REFERENCE_TIME rt)
{
    const int idx = SegmentAt(rt);
    const REFERENCE_TIME frame = FrameDuration();
    if (idx < 0) {
        Osd(_T("No segment to split here"));
        return;
    }
    auto segs = m_project.segments;
    const REFERENCE_TIME s = ToRT(segs[idx].start), e = ToRT(*segs[idx].end);
    if (rt - s < frame || e - rt < frame) {
        Osd(_T("Too close to the segment edge to split"));
        return;
    }
    llc::Segment second = segs[idx]; // keeps name, tags and export flag, like LosslessCut
    second.start = ToSeconds(rt);
    segs[idx].end = ToSeconds(rt);
    segs.insert(segs.begin() + idx + 1, std::move(second));
    CString osd;
    osd.Format(_T("Segment %d split at %s"), idx + 1, FormatTime(rt).GetString());
    Commit(std::move(segs), idx + 1, osd);
}

// -------------------------------------------------------------------------------------------------
// Context menu (seekbar)

void CLosslessCutController::ShowContextMenu(CWnd* pOwner, CPoint screenPt, REFERENCE_TIME rtAt)
{
    if (!m_bActive) {
        return;
    }
    enum {
        CMD_SET_START = 1, CMD_SET_END, CMD_SPLIT, CMD_ADD, CMD_SELECTED, CMD_REMOVE,
        CMD_SCOPE_FULL, CMD_SCOPE_SEGMENTS, CMD_SCOPE_SINGLE, CMD_RESTRICT, CMD_LOOP,
        CMD_UNDO, CMD_REDO, CMD_LAUNCH, CMD_OFF, CMD_LIST
    };

    const int hit = SegmentAt(rtAt);
    if (hit >= 0 && hit != m_iCurrent) {
        m_iCurrent = hit;
        NotifyChanged();
    }
    const llc::Segment* cur = GetCurrentSegment();
    const UINT edit = CanEdit() ? MF_ENABLED : MF_GRAYED;
    const CString at = FormatTime(rtAt);

    m_menu.DestroyMenu();
    m_menu.CreatePopupMenu();
    if (cur) {
        CString title;
        title.Format(_T("Segment %d"), m_iCurrent + 1);
        if (!cur->name.empty()) {
            title += _T(" - ") + W(cur->name);
        }
        m_menu.AppendMenu(MF_STRING | MF_GRAYED, 0, title);
        m_menu.AppendMenu(MF_SEPARATOR);
    }
    m_menu.AppendMenu(MF_STRING | edit, CMD_SET_START, _T("Set segment start at ") + at);
    m_menu.AppendMenu(MF_STRING | edit, CMD_SET_END, _T("Set segment end at ") + at);
    m_menu.AppendMenu(MF_STRING | (CanEdit() && hit >= 0 ? MF_ENABLED : MF_GRAYED), CMD_SPLIT, _T("Split segment at ") + at);
    m_menu.AppendMenu(MF_STRING | edit, CMD_ADD, _T("Add segment at ") + at);
    if (cur) {
        m_menu.AppendMenu(MF_STRING | edit | (cur->selected ? MF_CHECKED : MF_UNCHECKED), CMD_SELECTED, _T("Include in export"));
        m_menu.AppendMenu(MF_STRING | edit, CMD_REMOVE, _T("Remove segment"));
    }
    m_menu.AppendMenu(MF_SEPARATOR);
    const Scope scope = GetScope();
    m_menu.AppendMenu(MF_STRING | (scope == Scope::Full ? MF_CHECKED : MF_UNCHECKED), CMD_SCOPE_FULL, _T("Play everything"));
    m_menu.AppendMenu(MF_STRING | (scope == Scope::Segments ? MF_CHECKED : MF_UNCHECKED), CMD_SCOPE_SEGMENTS, _T("Play segments only"));
    m_menu.AppendMenu(MF_STRING | (scope == Scope::Single ? MF_CHECKED : MF_UNCHECKED) | (cur && !cur->IsMarker() ? MF_ENABLED : MF_GRAYED),
                      CMD_SCOPE_SINGLE, _T("Play this segment only"));
    m_menu.AppendMenu(MF_STRING | (IsRestricted() ? MF_CHECKED : MF_UNCHECKED), CMD_RESTRICT, _T("Keep seeking inside segments"));
    m_menu.AppendMenu(MF_STRING | (IsLooping() ? MF_CHECKED : MF_UNCHECKED), CMD_LOOP, _T("Loop"));
    m_menu.AppendMenu(MF_SEPARATOR);
    m_menu.AppendMenu(MF_STRING | (CanUndo() ? MF_ENABLED : MF_GRAYED), CMD_UNDO, _T("Undo"));
    m_menu.AppendMenu(MF_STRING | (CanRedo() ? MF_ENABLED : MF_GRAYED), CMD_REDO, _T("Redo"));
    m_menu.AppendMenu(MF_SEPARATOR);
    m_menu.AppendMenu(MF_STRING | (IsSegmentListShown() ? MF_CHECKED : MF_UNCHECKED), CMD_LIST, _T("Segment list"));
    m_menu.AppendMenu(MF_STRING, CMD_LAUNCH, _T("Open in LosslessCut"));
    m_menu.AppendMenu(MF_STRING, CMD_OFF, _T("Turn off LosslessCut mode"));

    m_menu.fulfillThemeReqs();
    const UINT cmd = m_menu.TrackPopupMenu(TPM_LEFTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, pOwner);
    m_menu.DestroyMenu();

    switch (cmd) {
        case CMD_SET_START: SetStart(rtAt); break;
        case CMD_SET_END: SetEnd(rtAt); break;
        case CMD_SPLIT: Split(rtAt); break;
        case CMD_ADD: {
            const REFERENCE_TIME saved = m_rtAddAt;
            m_rtAddAt = rtAt;
            OnAddSegment();
            m_rtAddAt = saved;
            break;
        }
        case CMD_SELECTED: OnToggleSelected(); break;
        case CMD_REMOVE: OnRemoveSegment(); break;
        case CMD_SCOPE_FULL: SetScope(Scope::Full); break;
        case CMD_SCOPE_SEGMENTS: SetScope(Scope::Segments); break;
        case CMD_SCOPE_SINGLE: SetScope(Scope::Single); break;
        case CMD_RESTRICT: OnToggleRestrict(); break;
        case CMD_LOOP: OnToggleLoop(); break;
        case CMD_UNDO: OnUndo(); break;
        case CMD_REDO: OnRedo(); break;
        case CMD_LAUNCH: OnLaunch(); break;
        case CMD_OFF: OnToggleMode(); break;
        case CMD_LIST: ShowSegmentList(!IsSegmentListShown()); break;
        default: break;
    }
}

// -------------------------------------------------------------------------------------------------
// Commands

BEGIN_MESSAGE_MAP(CLosslessCutController, CCmdTarget)
    ON_COMMAND(ID_LLC_TOGGLE_MODE, OnToggleMode)
    ON_UPDATE_COMMAND_UI(ID_LLC_TOGGLE_MODE, OnUpdateToggleMode)
    ON_COMMAND(ID_LLC_SET_START, OnSetStart)
    ON_COMMAND(ID_LLC_SET_END, OnSetEnd)
    ON_COMMAND(ID_LLC_ADD_SEGMENT, OnAddSegment)
    ON_COMMAND(ID_LLC_SPLIT_SEGMENT, OnSplitSegment)
    ON_COMMAND(ID_LLC_REMOVE_SEGMENT, OnRemoveSegment)
    ON_COMMAND(ID_LLC_TOGGLE_SELECTED, OnToggleSelected)
    ON_UPDATE_COMMAND_UI(ID_LLC_SET_START, OnUpdateEdit)
    ON_UPDATE_COMMAND_UI(ID_LLC_SET_END, OnUpdateEdit)
    ON_UPDATE_COMMAND_UI(ID_LLC_ADD_SEGMENT, OnUpdateEdit)
    ON_UPDATE_COMMAND_UI(ID_LLC_SPLIT_SEGMENT, OnUpdateEdit)
    ON_UPDATE_COMMAND_UI(ID_LLC_REMOVE_SEGMENT, OnUpdateEdit)
    ON_UPDATE_COMMAND_UI(ID_LLC_TOGGLE_SELECTED, OnUpdateEdit)
    ON_COMMAND(ID_LLC_PREV_SEGMENT, OnPrevSegment)
    ON_COMMAND(ID_LLC_NEXT_SEGMENT, OnNextSegment)
    ON_UPDATE_COMMAND_UI(ID_LLC_PREV_SEGMENT, OnUpdateNavigate)
    ON_UPDATE_COMMAND_UI(ID_LLC_NEXT_SEGMENT, OnUpdateNavigate)
    ON_COMMAND(ID_LLC_UNDO, OnUndo)
    ON_COMMAND(ID_LLC_REDO, OnRedo)
    ON_UPDATE_COMMAND_UI(ID_LLC_UNDO, OnUpdateUndo)
    ON_UPDATE_COMMAND_UI(ID_LLC_REDO, OnUpdateRedo)
    ON_COMMAND_RANGE(ID_LLC_SCOPE_CYCLE, ID_LLC_SCOPE_SINGLE, OnScope)
    ON_UPDATE_COMMAND_UI_RANGE(ID_LLC_SCOPE_CYCLE, ID_LLC_SCOPE_SINGLE, OnUpdateScope)
    ON_COMMAND(ID_LLC_TOGGLE_RESTRICT, OnToggleRestrict)
    ON_COMMAND(ID_LLC_TOGGLE_LOOP, OnToggleLoop)
    ON_UPDATE_COMMAND_UI(ID_LLC_TOGGLE_RESTRICT, OnUpdateToggles)
    ON_UPDATE_COMMAND_UI(ID_LLC_TOGGLE_LOOP, OnUpdateToggles)
    ON_COMMAND(ID_LLC_LAUNCH, OnLaunch)
    ON_UPDATE_COMMAND_UI(ID_LLC_LAUNCH, OnUpdateLaunch)
    ON_COMMAND(ID_LLC_TOGGLE_SEGMENT_LIST, OnToggleSegmentList)
    ON_UPDATE_COMMAND_UI(ID_LLC_TOGGLE_SEGMENT_LIST, OnUpdateToggleSegmentList)
END_MESSAGE_MAP()

void CLosslessCutController::OnToggleMode()
{
    // For the open file only: the next file opens in LosslessCut mode when it has a project.
    if (m_mediaPath.IsEmpty()) {
        Osd(_T("LosslessCut mode needs a local video file"));
        return;
    }
    SetActive(!m_bActive);
    if (m_bActive) {
        Osd(m_bFileKnown ? _T("LosslessCut mode on") : _T("LosslessCut mode on - the project file is created when you add a segment"), 3000);
    } else {
        Osd(_T("LosslessCut mode off"));
    }
}

void CLosslessCutController::OnUpdateToggleMode(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_mediaPath.IsEmpty());
    pCmdUI->SetCheck(m_bActive);
}

void CLosslessCutController::OnSetStart()
{
    if (CanEdit()) {
        SetStart(Position());
    }
}

void CLosslessCutController::OnSetEnd()
{
    if (CanEdit()) {
        SetEnd(Position());
    }
}

void CLosslessCutController::OnAddSegment()
{
    if (!CanEdit()) {
        return;
    }
    const REFERENCE_TIME t = (m_rtAddAt >= 0) ? m_rtAddAt : Position();
    const REFERENCE_TIME dur = Duration();
    REFERENCE_TIME end = t + DEFAULT_NEW_SEGMENT_LENGTH;
    if (dur > 0) {
        end = std::min(end, dur);
    }
    for (const auto& s : m_project.segments) {
        const REFERENCE_TIME start = ToRT(s.start);
        if (start > t + FrameDuration() / 2) {
            end = std::min(end, start);
        }
    }
    if (end - t < FrameDuration()) {
        Osd(_T("No room for a new segment here"));
        return;
    }
    auto segs = m_project.segments;
    llc::Segment seg;
    seg.start = ToSeconds(t);
    seg.end = ToSeconds(end);
    const int idx = InsertSorted(segs, std::move(seg));
    CString osd;
    osd.Format(_T("Segment %d added: %s - %s"), idx + 1, FormatTime(t).GetString(), FormatTime(end).GetString());
    Commit(std::move(segs), idx, osd);
}

void CLosslessCutController::OnSplitSegment()
{
    if (CanEdit()) {
        Split(Position());
    }
}

void CLosslessCutController::OnRemoveSegment()
{
    if (!CanEdit()) {
        return;
    }
    const llc::Segment* cur = GetCurrentSegment();
    if (!cur) {
        Osd(_T("No segment selected"));
        return;
    }
    auto segs = m_project.segments;
    const int removed = m_iCurrent;
    segs.erase(segs.begin() + removed);
    const int next = segs.empty() ? -1 : std::min(removed, static_cast<int>(segs.size()) - 1);
    CString osd;
    osd.Format(_T("Segment %d removed (undo with Ctrl+Z)"), removed + 1);
    Commit(std::move(segs), next, osd);
}

void CLosslessCutController::OnToggleSelected()
{
    ToggleSelected(m_iCurrent);
}

void CLosslessCutController::ToggleSelected(int index)
{
    if (!CanEdit() || index < 0 || index >= static_cast<int>(m_project.segments.size())) {
        return;
    }
    auto segs = m_project.segments;
    auto& seg = segs[index];
    seg.selected = !seg.selected;
    CString osd;
    osd.Format(seg.selected ? _T("Segment %d included in the export") : _T("Segment %d excluded from the export"), index + 1);
    Commit(std::move(segs), index, osd);
}

void CLosslessCutController::RenameSegment(int index, const CString& name)
{
    if (!CanEdit() || index < 0 || index >= static_cast<int>(m_project.segments.size())) {
        return;
    }
    CString trimmed = name;
    trimmed.Trim();
    const std::string utf8 = llc::WideToUtf8(std::wstring(trimmed));
    if (utf8 == m_project.segments[index].name) {
        return;
    }
    auto segs = m_project.segments;
    segs[index].name = utf8;
    CString osd;
    osd.Format(_T("Segment %d renamed"), index + 1);
    Commit(std::move(segs), index, osd);
}

void CLosslessCutController::OnToggleSegmentList()
{
    if (m_bActive) {
        ShowSegmentList(!IsSegmentListShown());
    }
}

void CLosslessCutController::OnUpdateToggleSegmentList(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bActive);
    pCmdUI->SetCheck(m_bActive && IsSegmentListShown());
}

void CLosslessCutController::OnUpdateEdit(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(CanEdit());
}

void CLosslessCutController::OnPrevSegment()
{
    if (!m_bActive) {
        return;
    }
    const auto& segs = m_project.segments;
    // While playing, a press shortly after a segment start goes to the one before, like "previous track".
    const REFERENCE_TIME threshold = IsRunning() ? ONE_SECOND / 2 : FrameDuration() / 2;
    const REFERENCE_TIME pos = Position();
    int best = -1;
    for (int i = 0; i < static_cast<int>(segs.size()); i++) {
        if (ToRT(segs[i].start) < pos - threshold && (best < 0 || segs[i].start > segs[best].start)) {
            best = i;
        }
    }
    if (best >= 0) {
        SelectSegment(best, true);
    }
}

void CLosslessCutController::OnNextSegment()
{
    if (!m_bActive) {
        return;
    }
    const auto& segs = m_project.segments;
    const REFERENCE_TIME pos = Position();
    int best = -1;
    for (int i = 0; i < static_cast<int>(segs.size()); i++) {
        if (ToRT(segs[i].start) > pos + FrameDuration() / 2 && (best < 0 || segs[i].start < segs[best].start)) {
            best = i;
        }
    }
    if (best >= 0) {
        SelectSegment(best, true);
    }
}

void CLosslessCutController::OnUpdateNavigate(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bActive && !m_project.segments.empty());
}

void CLosslessCutController::OnUndo()
{
    if (!CanUndo()) {
        if (m_bActive) {
            Osd(_T("Nothing to undo"));
        }
        return;
    }
    m_redo.push_back({ m_project.segments, m_iCurrent });
    Snapshot snap = std::move(m_undo.back());
    m_undo.pop_back();
    m_project.segments = std::move(snap.segments);
    m_iCurrent = snap.current;
    m_bDirty = true;
    const bool saved = Save();
    NotifyChanged();
    if (saved) {
        Osd(_T("Undo"));
    }
}

void CLosslessCutController::OnRedo()
{
    if (!CanRedo()) {
        if (m_bActive) {
            Osd(_T("Nothing to redo"));
        }
        return;
    }
    m_undo.push_back({ m_project.segments, m_iCurrent });
    Snapshot snap = std::move(m_redo.back());
    m_redo.pop_back();
    m_project.segments = std::move(snap.segments);
    m_iCurrent = snap.current;
    m_bDirty = true;
    const bool saved = Save();
    NotifyChanged();
    if (saved) {
        Osd(_T("Redo"));
    }
}

void CLosslessCutController::OnUpdateUndo(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(CanUndo());
}

void CLosslessCutController::OnUpdateRedo(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(CanRedo());
}

void CLosslessCutController::OnScope(UINT nID)
{
    if (!m_bActive) {
        return;
    }
    Scope scope;
    switch (nID) {
        case ID_LLC_SCOPE_FULL: scope = Scope::Full; break;
        case ID_LLC_SCOPE_SEGMENTS: scope = Scope::Segments; break;
        case ID_LLC_SCOPE_SINGLE: scope = Scope::Single; break;
        default:
            scope = static_cast<Scope>((static_cast<int>(GetScope()) + 1) % 3);
            break;
    }
    if (scope == Scope::Single) {
        const llc::Segment* cur = GetCurrentSegment();
        if (!cur || cur->IsMarker()) {
            const int idx = SegmentAt(Position());
            if (idx < 0) {
                Osd(_T("Move into a segment to play only that segment"));
                return;
            }
            m_iCurrent = idx;
        }
    }
    SetScope(scope);
}

void CLosslessCutController::OnUpdateScope(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bActive);
    if (pCmdUI->m_nID != ID_LLC_SCOPE_CYCLE) {
        const UINT current = (GetScope() == Scope::Full) ? ID_LLC_SCOPE_FULL : (GetScope() == Scope::Segments) ? ID_LLC_SCOPE_SEGMENTS : ID_LLC_SCOPE_SINGLE;
        pCmdUI->SetRadio(pCmdUI->m_nID == current);
    }
}

void CLosslessCutController::OnToggleRestrict()
{
    if (!m_bActive) {
        return;
    }
    auto& s = AfxGetAppSettings();
    s.bLosslessCutRestrict = !s.bLosslessCutRestrict;
    if (s.bLosslessCutRestrict && GetScope() == Scope::Full) {
        s.iLosslessCutScope = static_cast<int>(Scope::Segments); // restricting needs something to restrict to
        m_frame.AdjustStreamPosPoller(true);
    }
    Osd(s.bLosslessCutRestrict ? _T("Seeking stays inside the segments") : _T("Seeking anywhere"));
    if (s.bLosslessCutRestrict && m_frame.GetLoadState() == MLS::LOADED) {
        const REFERENCE_TIME pos = m_frame.GetPos();
        const REFERENCE_TIME clamped = ClampToRanges(pos);
        if (clamped != pos) {
            m_frame.DoSeekTo(clamped, false);
        }
    }
    NotifyChanged();
}

void CLosslessCutController::OnToggleLoop()
{
    if (!m_bActive) {
        return;
    }
    auto& s = AfxGetAppSettings();
    s.bLosslessCutLoop = !s.bLosslessCutLoop;
    Osd(s.bLosslessCutLoop ? _T("Loop on") : _T("Loop off"));
    NotifyChanged();
}

void CLosslessCutController::OnUpdateToggles(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bActive);
    pCmdUI->SetCheck(pCmdUI->m_nID == ID_LLC_TOGGLE_RESTRICT ? IsRestricted() : IsLooping());
}

bool CLosslessCutController::LocateLosslessCut(CString& exePath)
{
    auto& s = AfxGetAppSettings();
    exePath = s.strLosslessCutPath;
    if (!exePath.IsEmpty() && PathFileExists(exePath)) {
        return true;
    }

    // The installer's default location.
    TCHAR localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        const CString candidate = PathUtils::CombinePaths(localAppData, _T("Programs\\LosslessCut\\LosslessCut.exe"));
        if (PathFileExists(candidate)) {
            exePath = candidate;
            s.strLosslessCutPath = exePath;
            return true;
        }
    }

    CFileDialog dlg(TRUE, _T("exe"), _T("LosslessCut.exe"), OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY,
                    _T("LosslessCut|LosslessCut.exe|Programs (*.exe)|*.exe||"), &m_frame);
    dlg.m_ofn.lpstrTitle = _T("Where is LosslessCut.exe?");
    if (dlg.DoModal() != IDOK) {
        return false;
    }
    exePath = dlg.GetPathName();
    s.strLosslessCutPath = exePath;
    s.SaveSettings();
    return true;
}

void CLosslessCutController::OnLaunch()
{
    if (m_mediaPath.IsEmpty()) {
        Osd(_T("Open a local video file first"));
        return;
    }
    CString exe;
    if (!LocateLosslessCut(exe)) {
        return;
    }
    if (m_bDirty && !Save()) {
        if (AfxMessageBox(_T("The latest segment changes are not saved yet:\n") + m_lastError +
                          _T("\n\nLosslessCut will open the last saved version. Open it anyway?"), MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
    }
    if (IsRunning()) {
        m_frame.SendMessage(WM_COMMAND, ID_PLAY_PAUSE);
    }

    CString cmdLine;
    cmdLine.Format(_T("\"%s\" \"%s\""), exe.GetString(), m_mediaPath.GetString());
    CString dir = exe;
    PathRemoveFileSpec(dir.GetBuffer());
    dir.ReleaseBuffer();

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcess(exe, cmdLine.GetBuffer(), nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Osd(_T("Opening in LosslessCut"));
    } else {
        const DWORD err = GetLastError();
        CString msg;
        msg.Format(_T("LosslessCut could not be started (error %lu):\n%s\n\nThe path can be changed under Options > Advanced."), err, exe.GetString());
        AfxMessageBox(msg, MB_ICONERROR);
    }
    cmdLine.ReleaseBuffer();
}

void CLosslessCutController::OnUpdateLaunch(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_mediaPath.IsEmpty());
}
