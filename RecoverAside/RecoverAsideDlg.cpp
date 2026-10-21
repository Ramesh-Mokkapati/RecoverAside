// RecoverAsideDlg.cpp : main dialog - scanning, selection, and safe recovery.
//
// Safety rules enforced here:
//   1. The source volume is only ever opened GENERIC_READ (see NtfsVolume::Open).
//   2. Recovered data may only go to a volume on a *different physical disk*,
//      so freed clusters on the source disk can never be overwritten.
//   3. The destination must have enough free space for the selected data.
//   4. If no such destination exists, the user is asked to attach a USB / external drive.

#include "stdafx.h"
#include "RecoverAside.h"
#include "RecoverAsideDlg.h"
#include "DamagedFilesDlg.h"
#include <shlwapi.h>
#include <shellapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace
{
const UINT WM_SCAN_PROGRESS    = WM_APP + 1;
const UINT WM_SCAN_DONE        = WM_APP + 2;
const UINT WM_RECOVER_PROGRESS = WM_APP + 3;
const UINT WM_RECOVER_DONE     = WM_APP + 4;
const UINT WM_START_SCAN       = WM_APP + 5;

enum { COL_NAME, COL_PATH, COL_SIZE, COL_MODIFIED, COL_TYPE, COL_STATE };
enum { A_MOVEX = 1, A_MOVEY = 2, A_SIZEX = 4, A_SIZEY = 8 };

struct ScanJob
{
    HWND hwnd;
    CString device;
    std::atomic<bool>* cancel;
};

struct ScanResult
{
    std::unique_ptr<NtfsVolume> vol;
    std::vector<DeletedEntry> entries;
    std::vector<FolderInfo> folders;
    CString err;
    bool ok = false;
};

struct RecoverItem
{
    const DeletedEntry* e;
    CStringW rel;
};

struct RecoverJob
{
    HWND hwnd;
    NtfsVolume* vol;
    std::vector<RecoverItem> items;
    CStringW destBase;
    CStringW srcRoot;
    ULONGLONG totalBytes;
    std::atomic<bool>* cancel;
    std::vector<RecoverItem> userSkipped;   // damaged files unticked in the warning dialog
};

struct RecoverResult
{
    int ok = 0, warned = 0, failed = 0, skipped = 0, dirs = 0;
    ULONGLONG bytes = 0;
    bool cancelled = false;
    CStringW destBase;
    CString fatal;
};

CString FormatFileTime(const FILETIME& ft)
{
    if (!ft.dwLowDateTime && !ft.dwHighDateTime)
        return L"";
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return L"";
    CString s;
    s.Format(L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return s;
}

CStringW LongPath(const CStringW& p)
{
    return L"\\\\?\\" + p;
}

CStringW SanitizeComponent(const CStringW& in)
{
    CStringW s = in;
    for (int i = 0; i < s.GetLength(); ++i)
    {
        wchar_t c = s[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
            s.SetAt(i, L'_');
    }
    int n = s.GetLength();
    if (n && (s[n - 1] == L'.' || s[n - 1] == L' '))
        s.SetAt(n - 1, L'_');
    if (s.IsEmpty())
        s = L"_";

    static const wchar_t* reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    CStringW base = s;
    int dot = base.Find(L'.');
    if (dot >= 0)
        base = base.Left(dot);
    for (auto r : reserved)
        if (base.CompareNoCase(r) == 0)
            return L"_" + s;
    return s;
}

bool EnsureDir(const CStringW& path)
{
    // path is "X:\a\b..." - create each level.
    for (int i = 3; i <= path.GetLength(); ++i)
    {
        if (i == path.GetLength() || path[i] == L'\\')
        {
            if (!CreateDirectoryW(LongPath(path.Left(i)), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
    }
    return true;
}

HANDLE CreateUnique(const CStringW& full, CStringW& actual)
{
    int dot = full.ReverseFind(L'.');
    int slash = full.ReverseFind(L'\\');
    if (dot <= slash)
        dot = -1;
    CStringW stem = dot < 0 ? full : full.Left(dot);
    CStringW ext = dot < 0 ? CStringW() : full.Mid(dot);

    for (int n = 1; n < 10000; ++n)
    {
        if (n == 1)
            actual = full;
        else
            actual.Format(L"%s (%d)%s", (LPCWSTR)stem, n, (LPCWSTR)ext);
        HANDLE h = CreateFileW(LongPath(actual), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS)
            return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

CString StateText(const DeletedEntry& e)
{
    CString s;
    switch (e.state)
    {
    case RecoverState::Good:        s = L"Recoverable"; break;
    case RecoverState::Partial:     s.Format(L"Partially overwritten (%d%%)", e.overwrittenPct); break;
    case RecoverState::Overwritten: s = L"Overwritten"; break;
    case RecoverState::Unsupported: s = L"Not supported"; break;
    }
    if (!e.note.IsEmpty())
        s += L" - " + e.note;
    return s;
}

UINT ScanThread(LPVOID p)
{
    std::unique_ptr<ScanJob> job(static_cast<ScanJob*>(p));
    auto* res = new ScanResult;
    res->vol = std::make_unique<NtfsVolume>();
    int last = -1;
    if (res->vol->Open(job->device, res->err))
    {
        res->ok = res->vol->Scan(res->entries, res->folders, [&](ULONGLONG done, ULONGLONG total)
        {
            int pm = total ? (int)(done * 1000 / total) : 0;
            if (pm != last)
            {
                last = pm;
                ::PostMessage(job->hwnd, WM_SCAN_PROGRESS, pm, 0);
            }
            return !job->cancel->load();
        }, res->err);
    }
    ::PostMessage(job->hwnd, WM_SCAN_DONE, 0, (LPARAM)res);
    return 0;
}

UINT RecoverThread(LPVOID p)
{
    std::unique_ptr<RecoverJob> job(static_cast<RecoverJob*>(p));
    auto* res = new RecoverResult;
    res->destBase = job->destBase;

    // Last line of defence: never write onto the source volume.
    wchar_t volPath[MAX_PATH] = {};
    GetVolumePathNameW(job->destBase, volPath, MAX_PATH);
    if (_wcsicmp(volPath, job->srcRoot) == 0)
    {
        res->fatal = L"Refusing to write to the source volume.";
        ::PostMessage(job->hwnd, WM_RECOVER_DONE, 0, (LPARAM)res);
        return 0;
    }
    if (!EnsureDir(job->destBase))
    {
        res->fatal.Format(L"Cannot create %s (error %u).", (LPCWSTR)job->destBase, GetLastError());
        ::PostMessage(job->hwnd, WM_RECOVER_DONE, 0, (LPARAM)res);
        return 0;
    }

    CStringW log;
    log.Format(L"RecoverAside recovery log\r\nSource: %s\r\nDestination: %s\r\n\r\n",
               (LPCWSTR)job->srcRoot, (LPCWSTR)job->destBase);
    for (const RecoverItem& it : job->userSkipped)
    {
        CStringW line;
        line.Format(L"SKIP   %s (%s; skipped by user as damaged)\r\n", (LPCWSTR)it.rel, (LPCWSTR)StateText(*it.e));
        log += line;
        ++res->skipped;
    }

    ULONGLONG doneBytes = 0;
    int last = -1;
    for (const RecoverItem& it : job->items)
    {
        if (job->cancel->load())
        {
            res->cancelled = true;
            break;
        }
        const DeletedEntry& e = *it.e;
        CStringW full = job->destBase + it.rel;
        CStringW line;

        if (e.isDir)
        {
            if (EnsureDir(full)) ++res->dirs;
            else { ++res->failed; line.Format(L"FAIL   [folder] %s (error %u)\r\n", (LPCWSTR)it.rel, GetLastError()); }
            log += line;
            continue;
        }
        if (e.state == RecoverState::Unsupported)
        {
            ++res->skipped;
            line.Format(L"SKIP   %s (%s)\r\n", (LPCWSTR)it.rel, (LPCWSTR)StateText(e));
            log += line;
            continue;
        }

        EnsureDir(full.Left(full.ReverseFind(L'\\')));
        CStringW actual;
        HANDLE h = CreateUnique(full, actual);
        if (h == INVALID_HANDLE_VALUE)
        {
            ++res->failed;
            line.Format(L"FAIL   %s (cannot create file, error %u)\r\n", (LPCWSTR)it.rel, GetLastError());
            log += line;
            continue;
        }

        CString warn, err;
        bool ok = job->vol->ExtractTo(e, h, [&](ULONGLONG d, ULONGLONG)
        {
            int pm = job->totalBytes ? (int)((doneBytes + d) * 1000 / job->totalBytes) : 0;
            if (pm != last)
            {
                last = pm;
                ::PostMessage(job->hwnd, WM_RECOVER_PROGRESS, pm, 0);
            }
            return !job->cancel->load();
        }, warn, err);

        const FILETIME* ct = (e.created.dwHighDateTime || e.created.dwLowDateTime) ? &e.created : nullptr;
        const FILETIME* at = (e.accessed.dwHighDateTime || e.accessed.dwLowDateTime) ? &e.accessed : nullptr;
        const FILETIME* mt = (e.modified.dwHighDateTime || e.modified.dwLowDateTime) ? &e.modified : nullptr;
        SetFileTime(h, ct, at, mt);
        CloseHandle(h);

        CStringW shown = actual.Mid(job->destBase.GetLength());
        if (!ok)
        {
            if (job->cancel->load())
            {
                DeleteFileW(LongPath(actual));  // incomplete output on the destination only
                res->cancelled = true;
                break;
            }
            ++res->failed;
            line.Format(L"FAIL   %s (%s)\r\n", (LPCWSTR)shown, (LPCWSTR)err);
        }
        else if (!warn.IsEmpty() || e.state != RecoverState::Good)
        {
            ++res->warned;
            res->bytes += e.size;
            line.Format(L"WARN   %s  %llu bytes  (%s%s%s)\r\n", (LPCWSTR)shown, e.size, (LPCWSTR)StateText(e),
                        warn.IsEmpty() ? L"" : L"; ", (LPCWSTR)warn);
        }
        else
        {
            ++res->ok;
            res->bytes += e.size;
            line.Format(L"OK     %s  %llu bytes\r\n", (LPCWSTR)shown, e.size);
        }
        log += line;
        doneBytes += e.size;
    }

    CStringW summary;
    summary.Format(L"\r\nRecovered OK: %d, with warnings: %d, failed: %d, skipped: %d, folders: %d%s\r\n",
                   res->ok, res->warned, res->failed, res->skipped, res->dirs, res->cancelled ? L" (CANCELLED)" : L"");
    log += summary;

    HANDLE hLog = CreateFileW(LongPath(job->destBase + L"\\RecoveryLog.txt"), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WORD bom = 0xFEFF;
        WriteFile(hLog, &bom, sizeof(bom), &w, nullptr);
        WriteFile(hLog, (LPCWSTR)log, (DWORD)(log.GetLength() * sizeof(wchar_t)), &w, nullptr);
        CloseHandle(hLog);
    }

    ::PostMessage(job->hwnd, WM_RECOVER_DONE, 0, (LPARAM)res);
    return 0;
}
} // namespace

CRecoverAsideDlg::CRecoverAsideDlg(CWnd* pParent)
    : CDialogEx(IDD_RECOVERASIDE_DIALOG, pParent)
{
    m_hIcon = ::LoadIcon(nullptr, IDI_SHIELD);
}

void CRecoverAsideDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST, m_list);
    DDX_Control(pDX, IDC_TREE, m_tree);
    DDX_Control(pDX, IDC_SOURCE, m_cbSrc);
    DDX_Control(pDX, IDC_DEST, m_cbDst);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
}

BEGIN_MESSAGE_MAP(CRecoverAsideDlg, CDialogEx)
    ON_BN_CLICKED(IDC_SCAN, &CRecoverAsideDlg::OnScan)
    ON_BN_CLICKED(IDC_REFRESH, &CRecoverAsideDlg::OnRefresh)
    ON_BN_CLICKED(IDC_RECOVER, &CRecoverAsideDlg::OnRecover)
    ON_BN_CLICKED(IDC_STOP, &CRecoverAsideDlg::OnStop)
    ON_BN_CLICKED(IDC_CHECKALL, &CRecoverAsideDlg::OnCheckAll)
    ON_BN_CLICKED(IDC_UNCHECKALL, &CRecoverAsideDlg::OnUncheckAll)
    ON_BN_CLICKED(IDC_ONLYGOOD, &CRecoverAsideDlg::OnFilterChange)
    ON_EN_CHANGE(IDC_FILTER, &CRecoverAsideDlg::OnFilterChange)
    ON_CBN_SELCHANGE(IDC_SOURCE, &CRecoverAsideDlg::OnSourceChange)
    ON_CBN_SELCHANGE(IDC_DEST, &CRecoverAsideDlg::OnDestChange)
    ON_NOTIFY(LVN_GETDISPINFO, IDC_LIST, &CRecoverAsideDlg::OnGetDispInfo)
    ON_NOTIFY(NM_CLICK, IDC_LIST, &CRecoverAsideDlg::OnListClick)
    ON_NOTIFY(LVN_KEYDOWN, IDC_LIST, &CRecoverAsideDlg::OnListKeyDown)
    ON_NOTIFY(LVN_COLUMNCLICK, IDC_LIST, &CRecoverAsideDlg::OnColumnClick)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST, &CRecoverAsideDlg::OnListDblClick)
    ON_NOTIFY(TVN_ITEMEXPANDING, IDC_TREE, &CRecoverAsideDlg::OnTreeExpanding)
    ON_NOTIFY(TVN_SELCHANGED, IDC_TREE, &CRecoverAsideDlg::OnTreeSelChanged)
    ON_BN_CLICKED(IDC_BROWSE, &CRecoverAsideDlg::OnBrowseFolder)
    ON_BN_CLICKED(IDC_SUBFOLDERS, &CRecoverAsideDlg::OnScopeOptionChange)
    ON_BN_CLICKED(IDC_SHOWDELDIRS, &CRecoverAsideDlg::OnShowDeletedDirsChange)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_DEVICECHANGE()
    ON_MESSAGE(WM_SCAN_PROGRESS, &CRecoverAsideDlg::OnScanProgress)
    ON_MESSAGE(WM_SCAN_DONE, &CRecoverAsideDlg::OnScanDone)
    ON_MESSAGE(WM_RECOVER_PROGRESS, &CRecoverAsideDlg::OnRecoverProgress)
    ON_MESSAGE(WM_RECOVER_DONE, &CRecoverAsideDlg::OnRecoverDone)
    ON_MESSAGE(WM_START_SCAN, &CRecoverAsideDlg::OnStartScan)
END_MESSAGE_MAP()

BOOL CRecoverAsideDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    // Layout anchors for resizing.
    GetClientRect(&m_initClient);
    GetWindowRect(&m_initWindow);
    const std::pair<UINT, int> anchors[] = {
        { IDC_SOURCE, A_SIZEX }, { IDC_SCAN, A_MOVEX }, { IDC_REFRESH, A_MOVEX },
        { IDC_CHECKALL, A_MOVEX }, { IDC_UNCHECKALL, A_MOVEX },
        { IDC_TREE, A_SIZEY }, { IDC_LIST, A_SIZEX | A_SIZEY },
        { IDC_SCOPE, A_SIZEX }, { IDC_BROWSE, A_MOVEX }, { IDC_SUBFOLDERS, A_MOVEX }, { IDC_SHOWDELDIRS, A_MOVEX },
        { IDC_SELINFO, A_MOVEY | A_SIZEX },
        { IDC_STATIC_DEST, A_MOVEY }, { IDC_DEST, A_MOVEY | A_SIZEX }, { IDC_DESTINFO, A_MOVEY | A_SIZEX },
        { IDC_PROGRESS, A_MOVEY | A_SIZEX }, { IDC_STATUS, A_MOVEY | A_SIZEX },
        { IDC_STOP, A_MOVEX | A_MOVEY }, { IDC_RECOVER, A_MOVEX | A_MOVEY }, { IDCANCEL, A_MOVEX | A_MOVEY },
    };
    for (auto& a : anchors)
    {
        CRect rc;
        GetDlgItem(a.first)->GetWindowRect(&rc);
        ScreenToClient(&rc);
        m_anchors.push_back({ a.first, rc, a.second });
    }

    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    m_list.SetCallbackMask(LVIS_STATEIMAGEMASK);
    m_list.InsertColumn(COL_NAME, L"Name", LVCFMT_LEFT, 200);
    m_list.InsertColumn(COL_PATH, L"Original location", LVCFMT_LEFT, 260);
    m_list.InsertColumn(COL_SIZE, L"Size", LVCFMT_RIGHT, 80);
    m_list.InsertColumn(COL_MODIFIED, L"Modified", LVCFMT_LEFT, 110);
    m_list.InsertColumn(COL_TYPE, L"Type", LVCFMT_LEFT, 70);
    m_list.InsertColumn(COL_STATE, L"Recoverability", LVCFMT_LEFT, 200);

    // Tree icons: 0 drive, 1 live folder, 2 deleted folder, 3 unknown location.
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    m_treeImages.Create(cx, cy, ILC_COLOR32 | ILC_MASK, 4, 0);
    for (SHSTOCKICONID id : { SIID_DRIVEFIXED, SIID_FOLDER, SIID_RECYCLERFULL, SIID_WARNING })
    {
        SHSTOCKICONINFO sii = { sizeof(sii) };
        if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_SMALLICON, &sii)))
        {
            m_treeImages.Add(sii.hIcon);
            DestroyIcon(sii.hIcon);
        }
        else
            m_treeImages.Add(m_hIcon);
    }
    m_tree.SetImageList(&m_treeImages, TVSIL_NORMAL);
    CheckDlgButton(IDC_SUBFOLDERS, BST_CHECKED);
    CheckDlgButton(IDC_SHOWDELDIRS, BST_CHECKED);

    m_progress.SetRange32(0, 1000);
    RefreshVolumes();
    SetStatus(L"Pick a drive or folder in the tree; its drive is scanned automatically (read-only, never written).");
    return TRUE;
}

// ---------------------------------------------------------------- drives

void CRecoverAsideDlg::RefreshVolumes()
{
    CString prevSrc, prevDst;
    int s = m_cbSrc.GetCurSel();
    if (s >= 0) prevSrc = m_volumes[m_srcMap[s]].root;
    int d = SelectedDestVolume();
    if (d >= 0) prevDst = m_volumes[d].root;

    auto ntfsLetters = [this]()
    {
        CString s;
        for (const VolumeInfo& v : m_volumes)
            if (v.IsNtfs())
                s += v.root.Left(1);
        return s;
    };
    CString before = ntfsLetters();
    m_volumes = EnumerateVolumes();
    if (ntfsLetters() != before || m_folders.empty())
    {
        // Drives came or went: rebuild the tree, keeping the selected folder.
        ResetTree();
        SelectTreePath(m_scopeLetter, m_scope);
    }

    m_cbSrc.ResetContent();
    m_srcMap.clear();
    int sel = -1;
    for (int i = 0; i < (int)m_volumes.size(); ++i)
    {
        const VolumeInfo& v = m_volumes[i];
        if (!v.IsNtfs())
            continue;
        int k = m_cbSrc.AddString(v.Describe());
        m_srcMap.push_back(i);
        if (v.root.CompareNoCase(prevSrc) == 0 || (sel < 0 && prevSrc.IsEmpty() && !v.isSystem))
            sel = k;
    }
    if (sel < 0 && m_cbSrc.GetCount() > 0)
        sel = 0;
    m_cbSrc.SetCurSel(sel);

    FillDestinations(prevDst);
}

bool CRecoverAsideDlg::GetCheckSource(VolumeInfo& out) const
{
    if (m_haveScan)
    {
        out = m_scanSrc;
        return true;
    }
    int s = m_cbSrc.GetCurSel();
    if (s < 0)
        return false;
    out = m_volumes[m_srcMap[s]];
    return true;
}

int CRecoverAsideDlg::SelectedDestVolume() const
{
    int d = m_cbDst.GetSafeHwnd() ? m_cbDst.GetCurSel() : -1;
    return (d >= 0 && d < (int)m_dstMap.size()) ? m_dstMap[d] : -1;
}

ULONGLONG CRecoverAsideDlg::RequiredBytes(const VolumeInfo& dst, const SelStats& st)
{
    // Data, plus one cluster of slack and metadata per item, plus 1% and a 64 MB reserve.
    ULONGLONG cluster = dst.bytesPerCluster ? dst.bytesPerCluster : 4096;
    ULONGLONG data = st.bytes + (st.files + st.dirs + 1) * cluster;
    return data + data / 100 + 64ull * 1024 * 1024;
}

CString CRecoverAsideDlg::CheckDestination(const VolumeInfo& dst, const VolumeInfo& src, const SelStats& st)
{
    if (dst.root.CompareNoCase(src.root) == 0)
        return L"this is the source volume";
    if (dst.readOnly)
        return L"volume is read-only";
    if (!dst.disksKnown)
        return L"its physical disk cannot be identified";
    if (!src.disksKnown)
        return L"the source's physical disk cannot be identified";
    if (SharePhysicalDisk(dst, src))
        return L"same physical disk as the source - writing here could overwrite deleted data";
    if (dst.IsFat32() && st.largest > 0xFFFFFFFFull)
        return L"FAT32 cannot store files larger than 4 GB";
    ULONGLONG need = RequiredBytes(dst, st);
    if (dst.freeBytes < need)
        return L"not enough free space (needs " + FormatBytes(need) + L", has " + FormatBytes(dst.freeBytes) + L")";
    return L"";
}

int CRecoverAsideDlg::BestDestination(const VolumeInfo& src, const SelStats& st) const
{
    // Prefer USB / removable media, then the emptiest disk, then the one with the most free space.
    int best = -1;
    for (int i = 0; i < (int)m_volumes.size(); ++i)
    {
        const VolumeInfo& v = m_volumes[i];
        if (!CheckDestination(v, src, st).IsEmpty())
            continue;
        if (best < 0) { best = i; continue; }
        const VolumeInfo& b = m_volumes[best];
        bool vExt = v.isUsb || v.driveType == DRIVE_REMOVABLE;
        bool bExt = b.isUsb || b.driveType == DRIVE_REMOVABLE;
        if (vExt != bExt) { if (vExt) best = i; continue; }
        if (v.UsedBytes() != b.UsedBytes()) { if (v.UsedBytes() < b.UsedBytes()) best = i; continue; }
        if (v.freeBytes > b.freeBytes) best = i;
    }
    return best;
}

void CRecoverAsideDlg::FillDestinations(const CString& preferRoot)
{
    VolumeInfo src;
    bool haveSrc = GetCheckSource(src);
    SelStats st = CollectSelection();

    m_cbDst.ResetContent();
    m_dstMap.clear();
    int sel = -1;
    for (int i = 0; i < (int)m_volumes.size(); ++i)
    {
        const VolumeInfo& v = m_volumes[i];
        if (haveSrc && v.root.CompareNoCase(src.root) == 0)
            continue;
        CString text = v.Describe();
        CString reason = haveSrc ? CheckDestination(v, src, st) : CString();
        if (!reason.IsEmpty())
            text += L"     <- NOT USABLE: " + reason;
        int k = m_cbDst.AddString(text);
        m_dstMap.push_back(i);
        if (v.root.CompareNoCase(preferRoot) == 0)
            sel = k;
    }
    if (sel < 0 && haveSrc)
    {
        int best = BestDestination(src, st);
        for (int k = 0; k < (int)m_dstMap.size(); ++k)
            if (m_dstMap[k] == best)
                sel = k;
    }
    m_cbDst.SetCurSel(sel);
    UpdateDestInfo(st);
}

void CRecoverAsideDlg::UpdateDestInfo(const SelStats& st)
{
    VolumeInfo src;
    int d = SelectedDestVolume();
    CString info;
    if (!GetCheckSource(src))
        info = L"No NTFS source volume found.";
    else if (d < 0)
        info = L"No safe destination selected. Attach a USB / external drive (a different physical disk than the source).";
    else
    {
        const VolumeInfo& v = m_volumes[d];
        CString reason = CheckDestination(v, src, st);
        if (reason.IsEmpty())
            info = L"OK - " + v.root.Left(2) + L" is on a different physical disk than " + src.root.Left(2) +
                   L". Needs about " + FormatBytes(RequiredBytes(v, st)) + L", has " + FormatBytes(v.freeBytes) + L" free.";
        else
            info = L"NOT SAFE - " + v.root.Left(2) + L": " + reason + L".";
    }
    SetDlgItemText(IDC_DESTINFO, info);
}

void CRecoverAsideDlg::OnRefresh()
{
    if (m_busy)
        return;
    RefreshVolumes();
    SetStatus(L"Drive list refreshed.");
}

BOOL CRecoverAsideDlg::OnDeviceChange(UINT nEventType, DWORD_PTR)
{
    if ((nEventType == DBT_DEVICEARRIVAL || nEventType == DBT_DEVICEREMOVECOMPLETE) && !m_busy)
    {
        RefreshVolumes();
        SetStatus(nEventType == DBT_DEVICEARRIVAL ? L"New drive detected - drive list updated."
                                                  : L"Drive removed - drive list updated.");
    }
    return TRUE;
}

void CRecoverAsideDlg::OnSourceChange()
{
    int s = m_cbSrc.GetCurSel();
    if (s < 0)
        return;
    wchar_t letter = (wchar_t)towupper(m_volumes[m_srcMap[s]].root[0]);
    if (!m_haveScan)
    {
        int d = SelectedDestVolume();
        FillDestinations(d >= 0 ? m_volumes[d].root : CString());
    }
    SelectTreePath(letter, CStringW());
    OnFolderChosen(letter, CStringW());
}

void CRecoverAsideDlg::OnDestChange()
{
    UpdateDestInfo(CollectSelection());
}

// ---------------------------------------------------------------- scanning

void CRecoverAsideDlg::OnScan()
{
    if (m_busy)
        return;
    int s = m_cbSrc.GetCurSel();
    if (s < 0)
    {
        AfxMessageBox(L"No NTFS volume is available to scan.", MB_ICONINFORMATION);
        return;
    }
    wchar_t letter = (wchar_t)towupper(m_volumes[m_srcMap[s]].root[0]);
    if (letter != m_scopeLetter)
    {
        m_scopeLetter = letter;
        m_scope.Empty();
        SelectTreePath(letter, m_scope);
    }
    StartScan(letter, false);
}

void CRecoverAsideDlg::StartScan(wchar_t letter, bool automatic)
{
    const VolumeInfo* pv = VolumeFor(letter);
    if (m_busy || !pv || !pv->IsNtfs())
        return;
    const VolumeInfo v = *pv;     // m_volumes may be refreshed while the scan runs

    if (v.isSystem && m_systemScanOk.Find(letter) < 0)
    {
        if (AfxMessageBox(L"" + v.root.Left(2) + L" is the Windows system volume. Windows writes to it constantly, "
                          L"which can overwrite deleted files at any moment.\n\n"
                          L"For the best chance of recovery, connect this disk to another PC and scan it there.\n\n"
                          L"Scan anyway? (Scanning only reads the disk.)", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            if (automatic && m_declinedScan.Find(letter) < 0)
                m_declinedScan += letter;
            SetStatus(L"Scan skipped. You can still browse folders; click Scan to scan " + v.root.Left(2) + L".");
            return;
        }
        m_systemScanOk += letter;
    }
    m_declinedScan.Remove(letter);

    m_ntfs.reset();
    m_haveScan = false;
    m_entries.clear();
    m_scanFolders.clear();
    m_checked.clear();
    m_children.clear();
    m_folderItems.clear();
    m_view.clear();
    m_list.SetItemCountEx(0);
    ResetTree();    // drop the previous drive's deleted folders and counts
    SelectTreePath(m_scopeLetter, m_scope);
    OnSelectionChanged();

    m_pendingSrc = v;
    m_cancel = false;
    auto* job = new ScanJob{ GetSafeHwnd(), v.device, &m_cancel };
    SetBusy(true, L"Scanning the Master File Table of " + v.root.Left(2) + L" (read-only)...");
    if (!AfxBeginThread(ScanThread, job))
    {
        delete job;
        SetBusy(false, L"Could not start the scan thread.");
    }
}

LRESULT CRecoverAsideDlg::OnScanProgress(WPARAM wParam, LPARAM)
{
    m_progress.SetPos((int)wParam);
    CString s;
    s.Format(L"Scanning %s ... %d%%", (LPCWSTR)m_pendingSrc.root.Left(2), (int)wParam / 10);
    SetStatus(s);
    return 0;
}

LRESULT CRecoverAsideDlg::OnScanDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<ScanResult> res(reinterpret_cast<ScanResult*>(lParam));
    SetBusy(false, L"");
    if (m_closePending)
    {
        EndDialog(IDCANCEL);
        return 0;
    }
    if (!res->ok)
    {
        SetStatus(res->err);
        if (!m_cancel)
            AfxMessageBox(res->err, MB_ICONERROR);
        return 0;
    }

    m_ntfs = std::move(res->vol);
    m_entries = std::move(res->entries);
    m_scanFolders = std::move(res->folders);
    m_scanSrc = m_pendingSrc;
    m_haveScan = true;
    m_checked.assign(m_entries.size(), 0);
    BuildChildren();
    ComputeFolderSizes();

    // Show the scanned drive's results in the folder the user picked (or its root).
    if (m_scopeLetter != ScanLetter())
    {
        m_scopeLetter = ScanLetter();
        m_scope.Empty();
    }
    ResetTree();
    SelectTreePath(m_scopeLetter, m_scope);
    SetScope(m_scope);

    size_t files = 0, dirs = 0, good = 0;
    for (const DeletedEntry& e : m_entries)
    {
        if (e.isDir) ++dirs;
        else { ++files; if (e.state == RecoverState::Good) ++good; }
    }
    CString s;
    s.Format(L"Found %zu deleted files (%zu fully recoverable) and %zu deleted folders on %s.",
             files, good, dirs, (LPCWSTR)m_scanSrc.root.Left(2));
    SetStatus(s);
    m_progress.SetPos(1000);

    int d = SelectedDestVolume();
    FillDestinations(d >= 0 ? m_volumes[d].root : CString());

    if (BestDestination(m_scanSrc, SelStats()) < 0)
    {
        AfxMessageBox(L"Scan complete.\n\nThere is currently NO safe place to write recovered files: every other "
                      L"volume is on the same physical disk as " + m_scanSrc.root.Left(2) + L", read-only, or full.\n\n"
                      L"Before recovering, attach a USB flash drive or external disk. It will be detected automatically.",
                      MB_ICONWARNING);
    }
    return 0;
}

// ---------------------------------------------------------------- results

bool CRecoverAsideDlg::IsChildOf(const DeletedEntry& child, const DeletedEntry& dir) const
{
    // The folder's sequence number was bumped by one when it was deleted.
    return child.parentRecord == dir.recordNo &&
           (child.parentSeq == dir.seq || (WORD)(child.parentSeq + 1) == dir.seq || child.parentSeq == 0);
}

void CRecoverAsideDlg::BuildChildren()
{
    m_children.clear();
    for (int i = 0; i < (int)m_entries.size(); ++i)
        m_children[m_entries[i].parentRecord].push_back(i);
}

void CRecoverAsideDlg::ComputeFolderSizes()
{
    // Iterative post-order walk so deep trees cannot overflow the stack.
    m_folderItems.assign(m_entries.size(), 0);
    std::vector<char> state(m_entries.size(), 0);   // 0 new, 1 open, 2 done
    for (int root = 0; root < (int)m_entries.size(); ++root)
    {
        if (!m_entries[root].isDir || state[root])
            continue;
        std::vector<int> stack{ root };
        while (!stack.empty())
        {
            int i = stack.back();
            DeletedEntry& dir = m_entries[i];
            auto it = m_children.find(dir.recordNo);
            if (state[i] == 0)
            {
                state[i] = 1;
                if (it != m_children.end())
                    for (int c : it->second)
                        if (m_entries[c].isDir && state[c] == 0 && IsChildOf(m_entries[c], dir))
                            stack.push_back(c);
                continue;
            }
            stack.pop_back();
            if (state[i] == 2)
                continue;
            state[i] = 2;
            dir.size = 0;
            if (it != m_children.end())
                for (int c : it->second)
                {
                    if (c == i || !IsChildOf(m_entries[c], dir))
                        continue;
                    if (m_entries[c].isDir && state[c] != 2)
                        continue;   // cycle guard
                    dir.size += m_entries[c].size;
                    m_folderItems[i] += 1 + m_folderItems[c];
                }
        }
    }
}

CString CRecoverAsideDlg::FilterText() const
{
    CString filter;
    GetDlgItemText(IDC_FILTER, filter);
    filter.Trim();
    return filter;
}

bool CRecoverAsideDlg::OnlyGood() const
{
    return IsDlgButtonChecked(IDC_ONLYGOOD) == BST_CHECKED;
}

bool CRecoverAsideDlg::MatchesText(const DeletedEntry& e, const CString& filter)
{
    return filter.IsEmpty() || StrStrIW(e.name, filter) || StrStrIW(e.path, filter);
}

void CRecoverAsideDlg::RebuildView()
{
    CString filter = FilterText();
    bool onlyGood = OnlyGood();
    bool subfolders = IsDlgButtonChecked(IDC_SUBFOLDERS) == BST_CHECKED;
    // Only the scanned drive has results; another drive's folder shows nothing until it is scanned.
    bool active = m_haveScan && m_scopeLetter == ScanLetter();

    m_view.clear();
    m_view.reserve(m_entries.size());
    m_hiddenByGood = m_hiddenByText = 0;
    for (int i = 0; active && i < (int)m_entries.size(); ++i)
    {
        const DeletedEntry& e = m_entries[i];
        if (!InScope(e, subfolders))
            continue;
        if (onlyGood && e.state != RecoverState::Good)
        {
            ++m_hiddenByGood;
            continue;
        }
        if (!MatchesText(e, filter))
        {
            ++m_hiddenByText;
            continue;
        }
        m_view.push_back(i);
    }
    SortView();
    m_list.SetItemCountEx((int)m_view.size(), LVSICF_NOSCROLL);
    m_list.Invalidate();
}

// ---------------------------------------------------------------- folder tree / scope
//
// The tree works like Explorer's: every NTFS drive, with its existing folders listed when expanded.
// Selecting a folder scans its drive (read-only) if needed. Once a drive is scanned, folders that held
// deleted items show a count, and deleted folders found by the scan appear next to the live ones.

namespace
{
std::wstring NodeKey(wchar_t letter, const CStringW& path)
{
    CStringW k;
    k.Format(L"%c%s", letter, (LPCWSTR)path);
    k.MakeLower();
    return std::wstring(k);
}
}

wchar_t CRecoverAsideDlg::ScanLetter() const
{
    return m_haveScan ? (wchar_t)towupper(m_scanSrc.root[0]) : 0;
}

const VolumeInfo* CRecoverAsideDlg::VolumeFor(wchar_t letter) const
{
    for (const VolumeInfo& v : m_volumes)
        if ((wchar_t)towupper(v.root[0]) == letter)
            return &v;
    return nullptr;
}

bool CRecoverAsideDlg::InScope(const DeletedEntry& e, bool subfolders) const
{
    int len = m_scope.GetLength();
    if (!subfolders)
        return e.path.CompareNoCase(m_scope) == 0;
    if (len == 0)
        return true;
    return e.path.GetLength() >= len && _wcsnicmp(e.path, m_scope, len) == 0 &&
           (e.path.GetLength() == len || e.path[len] == L'\\');
}

int CRecoverAsideDlg::RootNodeFor(wchar_t letter) const
{
    auto it = m_folderIndex.find(NodeKey(letter, CStringW()));
    return it == m_folderIndex.end() ? -1 : it->second;
}

int CRecoverAsideDlg::FolderNodeFor(wchar_t letter, const CStringW& path)
{
    std::wstring key = NodeKey(letter, path);
    auto it = m_folderIndex.find(key);
    if (it != m_folderIndex.end())
        return it->second;
    if (path.IsEmpty())
        return -1;      // drives are only created by ResetTree

    int slash = path.ReverseFind(L'\\');
    int parent = FolderNodeFor(letter, slash > 0 ? path.Left(slash) : CStringW());
    if (parent < 0)
        return -1;
    FolderNode n;
    n.letter = letter;
    n.name = path.Mid(slash + 1);
    n.path = path;
    n.parent = parent;
    m_folders.push_back(n);
    int idx = (int)m_folders.size() - 1;
    m_folders[parent].children.push_back(idx);
    m_folderIndex[key] = idx;
    return idx;
}

void CRecoverAsideDlg::EnumerateLive(int node)
{
    // Lists folder names only (a directory read); nothing is written to the drive.
    m_folders[node].enumerated = true;
    wchar_t letter = m_folders[node].letter;
    CStringW base = m_folders[node].path;
    CStringW pattern;
    pattern.Format(L"\\\\?\\%c:%s\\*", letter, (LPCWSTR)base);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;     // e.g. access denied
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        // Skip junctions and symlinks (e.g. "Documents and Settings") so each folder appears once.
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK))
            continue;
        int c = FolderNodeFor(letter, base + L"\\" + fd.cFileName);
        if (c >= 0)
            m_folders[c].state = FolderState::Live;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CRecoverAsideDlg::ApplyScanOverlay()
{
    wchar_t s = ScanLetter();
    if (!s || RootNodeFor(s) < 0)
        return;

    for (const FolderInfo& f : m_scanFolders)
    {
        int n = FolderNodeFor(s, f.path);
        if (n < 0)
            continue;
        if (f.live)
            m_folders[n].state = FolderState::Live;
        else if (m_folders[n].state != FolderState::Live)
            m_folders[n].state = FolderState::Deleted;
    }
    m_entryNode.assign(m_entries.size(), -1);
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const DeletedEntry& e = m_entries[i];
        if (e.isDir)
        {
            int n = FolderNodeFor(s, e.path + L"\\" + e.name);
            if (n >= 0 && m_folders[n].state == FolderState::Virtual)
                m_folders[n].state = FolderState::Deleted;
        }
        else
        {
            int n = FolderNodeFor(s, e.path);
            m_entryNode[i] = n;
            if (n >= 0)
                ++m_folders[n].deletedFiles;
        }
    }
    // Children are always created after their parent, so a reverse pass rolls counts up.
    for (int i = (int)m_folders.size() - 1; i >= 0; --i)
        if (m_folders[i].letter == s && m_folders[i].parent >= 0)
            m_folders[m_folders[i].parent].deletedFiles += m_folders[i].deletedFiles;
    ComputeShownCounts();
}

void CRecoverAsideDlg::ComputeShownCounts()
{
    // How many deleted files under each folder pass the current filters (the "1" in "1 of 3").
    wchar_t s = ScanLetter();
    for (FolderNode& f : m_folders)
        f.shownFiles = 0;
    if (!s)
        return;
    CString filter = FilterText();
    bool onlyGood = OnlyGood();
    for (size_t i = 0; i < m_entries.size() && i < m_entryNode.size(); ++i)
    {
        const DeletedEntry& e = m_entries[i];
        int n = m_entryNode[i];
        if (n < 0 || e.isDir || (onlyGood && e.state != RecoverState::Good) || !MatchesText(e, filter))
            continue;
        ++m_folders[n].shownFiles;
    }
    for (int i = (int)m_folders.size() - 1; i >= 0; --i)
        if (m_folders[i].letter == s && m_folders[i].parent >= 0)
            m_folders[m_folders[i].parent].shownFiles += m_folders[i].shownFiles;
}

void CRecoverAsideDlg::RefreshTreeLabels()
{
    for (int i = 0; i < (int)m_folders.size(); ++i)
        if (m_folders[i].item)
            m_tree.SetItemText(m_folders[i].item, NodeLabel(i));
}

bool CRecoverAsideDlg::IsNodeVisible(int node) const
{
    const FolderNode& f = m_folders[node];
    if (f.state == FolderState::Live)
        return true;
    if (f.letter != ScanLetter())
        return false;
    return f.state != FolderState::Deleted || IsDlgButtonChecked(IDC_SHOWDELDIRS) == BST_CHECKED;
}

CString CRecoverAsideDlg::NodeLabel(int node) const
{
    const FolderNode& f = m_folders[node];
    CString s;
    if (f.parent < 0)
    {
        const VolumeInfo* v = VolumeFor(f.letter);
        s.Format(L"%s (%c:)", (v && !v->label.IsEmpty()) ? (LPCWSTR)v->label : L"Local Disk", f.letter);
    }
    else
        s = f.name;

    if (f.letter == ScanLetter() && f.deletedFiles)
    {
        CString c;
        if (OnlyGood() || !FilterText().IsEmpty())   // filters on: "shown of total"
            c.Format(f.parent < 0 ? L"  (%zu of %zu deleted files)" : L"  (%zu of %zu)", f.shownFiles, f.deletedFiles);
        else
            c.Format(f.parent < 0 ? L"  (%zu deleted files)" : L"  (%zu)", f.deletedFiles);
        s += c;
    }
    if (f.state == FolderState::Deleted)
        s += L"  [deleted]";
    else if (f.state == FolderState::Virtual)
        s += L"  [location unknown]";
    return s;
}

void CRecoverAsideDlg::InsertNode(HTREEITEM parentItem, int node)
{
    const FolderNode& f = m_folders[node];
    bool hasKids = (f.state == FolderState::Live && !f.enumerated) ||
                   std::any_of(f.children.begin(), f.children.end(), [this](int k) { return IsNodeVisible(k); });
    int image = f.parent < 0 ? 0 : f.state == FolderState::Live ? 1 : f.state == FolderState::Deleted ? 2 : 3;
    CString label = NodeLabel(node);

    TVINSERTSTRUCT tis = {};
    tis.hParent = parentItem;
    tis.hInsertAfter = TVI_LAST;
    tis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    tis.item.pszText = const_cast<LPWSTR>((LPCWSTR)label);
    tis.item.iImage = tis.item.iSelectedImage = image;
    tis.item.cChildren = hasKids ? 1 : 0;
    tis.item.lParam = node;
    m_folders[node].item = m_tree.InsertItem(&tis);
}

void CRecoverAsideDlg::PopulateChildren(int node)
{
    if (m_folders[node].populated || !m_folders[node].item)
        return;
    m_folders[node].populated = true;
    if (m_folders[node].state == FolderState::Live && !m_folders[node].enumerated)
        EnumerateLive(node);    // may add nodes, so no references are held across this call

    std::vector<int> kids = m_folders[node].children;
    std::sort(kids.begin(), kids.end(), [this](int a, int b)
    {
        return m_folders[a].name.CompareNoCase(m_folders[b].name) < 0;
    });
    HTREEITEM parentItem = m_folders[node].item;
    int inserted = 0;
    for (int c : kids)
    {
        if (!IsNodeVisible(c))
            continue;
        InsertNode(parentItem, c);
        ++inserted;
    }
    if (!inserted)
    {
        TVITEM ti = {};
        ti.mask = TVIF_HANDLE | TVIF_CHILDREN;
        ti.hItem = parentItem;
        ti.cChildren = 0;
        m_tree.SetItem(&ti);
    }
}

void CRecoverAsideDlg::ResetTree()
{
    m_suppressTreeSel = true;
    m_tree.DeleteAllItems();
    m_folders.clear();
    m_folderIndex.clear();

    for (const VolumeInfo& v : m_volumes)
    {
        if (!v.IsNtfs())
            continue;
        FolderNode r;
        r.letter = (wchar_t)towupper(v.root[0]);
        r.name = v.root.Left(2);
        r.state = FolderState::Live;
        m_folders.push_back(r);
        m_folderIndex[NodeKey(r.letter, CStringW())] = (int)m_folders.size() - 1;
    }
    size_t drives = m_folders.size();
    ApplyScanOverlay();
    for (size_t i = 0; i < drives; ++i)
        InsertNode(TVI_ROOT, (int)i);
    m_suppressTreeSel = false;
}

void CRecoverAsideDlg::SelectTreePath(wchar_t letter, const CStringW& path)
{
    int node = RootNodeFor(letter);
    if (node < 0 || !m_folders[node].item)
        return;
    // Expand down to the deepest folder on the path that is in the tree.
    CStringW cur;
    int pos = 0;
    CStringW tok = path.Tokenize(L"\\", pos);
    while (!tok.IsEmpty())
    {
        cur += L"\\" + tok;
        PopulateChildren(node);
        auto it = m_folderIndex.find(NodeKey(letter, cur));
        if (it == m_folderIndex.end() || !m_folders[it->second].item)
            break;
        m_tree.Expand(m_folders[node].item, TVE_EXPAND);
        node = it->second;
        tok = path.Tokenize(L"\\", pos);
    }
    m_suppressTreeSel = true;
    m_tree.SelectItem(m_folders[node].item);
    m_tree.EnsureVisible(m_folders[node].item);
    m_suppressTreeSel = false;
}

void CRecoverAsideDlg::SetScope(const CStringW& path)
{
    m_scope = path;
    LPCWSTR shownPath = path.IsEmpty() ? L"\\" : (LPCWSTR)path;
    CString shown;
    if (m_scopeLetter)
        shown.Format(L"%c:%s", m_scopeLetter, shownPath);
    auto it = m_folderIndex.find(NodeKey(m_scopeLetter, path));
    if (!path.IsEmpty() && it != m_folderIndex.end() && m_folders[it->second].state == FolderState::Deleted)
        shown += L"   (deleted folder)";
    SetDlgItemText(IDC_SCOPE, shown);
    RebuildView();

    if (m_busy || !m_scopeLetter)
        return;
    // Say which filter hides how many items, so an empty list is never mistaken for "nothing found".
    CString hidden;
    if (m_hiddenByGood)
        hidden.Format(L"%zu hidden by \"Only show fully recoverable\"", m_hiddenByGood);
    if (m_hiddenByText)
    {
        CString t;
        t.Format(L"%s%zu hidden by the filter \"%s\"", hidden.IsEmpty() ? L"" : L"; ",
                 m_hiddenByText, (LPCWSTR)FilterText());
        hidden += t;
    }
    LPCWSTR where = IsDlgButtonChecked(IDC_SUBFOLDERS) == BST_CHECKED ? L" and its subfolders" : L"";

    CString s;
    if (m_scopeLetter != ScanLetter())
        s.Format(L"%c: has not been scanned yet. Click Scan to find deleted items.", m_scopeLetter);
    else if (m_view.empty() && hidden.IsEmpty())
        s.Format(L"No deleted items found in %c:%s%s.", m_scopeLetter, shownPath, where);
    else if (m_view.empty())
        s.Format(L"No deleted items shown in %c:%s%s: %s. Change the filter to see them.",
                 m_scopeLetter, shownPath, where, (LPCWSTR)hidden);
    else if (!hidden.IsEmpty())
        s.Format(L"Showing %zu deleted item(s) in %c:%s%s (%s). Double-click a folder to open it.",
                 m_view.size(), m_scopeLetter, shownPath, where, (LPCWSTR)hidden);
    else
        s.Format(L"Showing %zu deleted item(s) in %c:%s%s. Double-click a folder to open it.",
                 m_view.size(), m_scopeLetter, shownPath, where);
    SetStatus(s);
}

void CRecoverAsideDlg::OnFolderChosen(wchar_t letter, const CStringW& path)
{
    m_scopeLetter = letter;
    SetScope(path);
    if (letter == ScanLetter() || m_busy)
        return;

    // Keep the source drop-down in step with the tree.
    for (int k = 0; k < (int)m_srcMap.size(); ++k)
        if ((wchar_t)towupper(m_volumes[m_srcMap[k]].root[0]) == letter)
            m_cbSrc.SetCurSel(k);

    if (m_declinedScan.Find(letter) < 0)
        PostMessage(WM_START_SCAN, letter, 0);  // not from inside the tree notification
}

LRESULT CRecoverAsideDlg::OnStartScan(WPARAM wParam, LPARAM)
{
    wchar_t letter = (wchar_t)wParam;
    if (!m_busy && letter == m_scopeLetter && letter != ScanLetter())
        StartScan(letter, true);
    return 0;
}

void CRecoverAsideDlg::OnTreeExpanding(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* tv = reinterpret_cast<NMTREEVIEW*>(pNMHDR);
    if (tv->action == TVE_EXPAND && tv->itemNew.lParam >= 0 && tv->itemNew.lParam < (LPARAM)m_folders.size())
    {
        CWaitCursor wait;
        PopulateChildren((int)tv->itemNew.lParam);
    }
    *pResult = 0;
}

void CRecoverAsideDlg::OnTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* tv = reinterpret_cast<NMTREEVIEW*>(pNMHDR);
    *pResult = 0;
    if (m_suppressTreeSel || !tv->itemNew.hItem)
        return;
    LPARAM n = tv->itemNew.lParam;
    if (n >= 0 && n < (LPARAM)m_folders.size())
    {
        const FolderNode& f = m_folders[(size_t)n];
        OnFolderChosen(f.letter, f.path);
    }
}

void CRecoverAsideDlg::OnListDblClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    *pResult = 0;
    if (ia->iItem < 0 || ia->iItem >= (int)m_view.size())
        return;
    const DeletedEntry& e = m_entries[m_view[ia->iItem]];
    if (!e.isDir)
        return;
    CStringW path = e.path + L"\\" + e.name;
    SelectTreePath(ScanLetter(), path);
    SetScope(path);
}

void CRecoverAsideDlg::OnBrowseFolder()
{
    // The picker lists existing folders only; deleted ones are browsed in the tree after the scan.
    CString start;
    if (m_scopeLetter)
    {
        start.Format(L"%c:%s\\", m_scopeLetter, (LPCWSTR)m_scope);
        if (GetFileAttributesW(start) == INVALID_FILE_ATTRIBUTES)
            start.Format(L"%c:\\", m_scopeLetter);
    }
    CFolderPickerDialog dlg(start.IsEmpty() ? nullptr : (LPCWSTR)start, 0, this);
    if (dlg.DoModal() != IDOK)
        return;

    CString picked = dlg.GetFolderPath();
    wchar_t letter = picked.GetLength() >= 2 && picked[1] == L':' ? (wchar_t)towupper(picked[0]) : 0;
    if (!letter || RootNodeFor(letter) < 0)
    {
        AfxMessageBox(L"Please choose a folder on a local NTFS drive.", MB_ICONINFORMATION);
        return;
    }
    CStringW rel = picked.Mid(2);
    rel.TrimRight(L'\\');
    SelectTreePath(letter, rel);
    OnFolderChosen(letter, rel);
}

void CRecoverAsideDlg::OnScopeOptionChange()
{
    SetScope(m_scope);
}

void CRecoverAsideDlg::OnShowDeletedDirsChange()
{
    ResetTree();
    SelectTreePath(m_scopeLetter, m_scope);
}

void CRecoverAsideDlg::SortView()
{
    auto cmp = [this](int a, int b)
    {
        const DeletedEntry& x = m_entries[a];
        const DeletedEntry& y = m_entries[b];
        int r = 0;
        switch (m_sortCol)
        {
        case COL_NAME:     r = x.name.CompareNoCase(y.name); break;
        case COL_PATH:     r = x.path.CompareNoCase(y.path); if (!r) r = x.name.CompareNoCase(y.name); break;
        case COL_SIZE:     r = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
        case COL_MODIFIED: r = CompareFileTime(&x.modified, &y.modified); break;
        case COL_TYPE:     r = (int)y.isDir - (int)x.isDir; if (!r) r = CompareStringOrdinal(PathFindExtensionW(x.name), -1, PathFindExtensionW(y.name), -1, TRUE) - 2; break;
        case COL_STATE:    r = (int)x.state - (int)y.state; if (!r) r = x.overwrittenPct - y.overwrittenPct; break;
        }
        return m_sortAsc ? r < 0 : r > 0;
    };
    std::stable_sort(m_view.begin(), m_view.end(), cmp);
}

void CRecoverAsideDlg::OnColumnClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* lv = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    if (lv->iSubItem == m_sortCol)
        m_sortAsc = !m_sortAsc;
    else
    {
        m_sortCol = lv->iSubItem;
        m_sortAsc = true;
    }
    SortView();
    m_list.Invalidate();
    *pResult = 0;
}

void CRecoverAsideDlg::OnFilterChange()
{
    ComputeShownCounts();
    RefreshTreeLabels();
    SetScope(m_scope);      // rebuilds the list and explains anything the filters hide
}

CString CRecoverAsideDlg::CellText(int idx, int col) const
{
    const DeletedEntry& e = m_entries[idx];
    CString s;
    switch (col)
    {
    case COL_NAME:
        return e.name;
    case COL_PATH:
        return m_scanSrc.root.Left(2) + (e.path.IsEmpty() ? CStringW(L"\\") : e.path);
    case COL_SIZE:
        return (e.isDir && !e.size) ? CString() : FormatBytes(e.size);
    case COL_MODIFIED:
        return FormatFileTime(e.modified);
    case COL_TYPE:
        if (e.isDir)
            return L"Folder";
        s = PathFindExtensionW(e.name);
        if (s.GetLength() > 1)
        {
            s = s.Mid(1);
            s.MakeUpper();
            return s + L" file";
        }
        return L"File";
    case COL_STATE:
        if (e.isDir)
        {
            s.Format(L"Folder - %zu deleted item(s) inside", m_folderItems[idx]);
            return s;
        }
        return StateText(e);
    }
    return s;
}

void CRecoverAsideDlg::OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* di = reinterpret_cast<NMLVDISPINFO*>(pNMHDR);
    LVITEM& it = di->item;
    *pResult = 0;
    if (it.iItem < 0 || it.iItem >= (int)m_view.size())
        return;
    int idx = m_view[it.iItem];
    if ((it.mask & LVIF_TEXT) && it.pszText && it.cchTextMax > 0)
        wcsncpy_s(it.pszText, it.cchTextMax, CellText(idx, it.iSubItem), _TRUNCATE);
    if (it.mask & LVIF_STATE)
    {
        it.stateMask = LVIS_STATEIMAGEMASK;
        it.state = INDEXTOSTATEIMAGEMASK(m_checked[idx] ? 2 : 1);
    }
}

void CRecoverAsideDlg::ToggleViewItems(const std::vector<int>& rows)
{
    if (rows.empty() || m_busy)
        return;
    char newState = m_checked[m_view[rows[0]]] ? 0 : 1;
    for (int r : rows)
    {
        m_checked[m_view[r]] = newState;
        m_list.RedrawItems(r, r);
    }
    OnSelectionChanged();
}

void CRecoverAsideDlg::OnListClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    LVHITTESTINFO ht = {};
    ht.pt = ia->ptAction;
    m_list.HitTest(&ht);
    if (ht.iItem >= 0 && (ht.flags & LVHT_ONITEMSTATEICON))
        ToggleViewItems({ ht.iItem });
    *pResult = 0;
}

void CRecoverAsideDlg::OnListKeyDown(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* kd = reinterpret_cast<NMLVKEYDOWN*>(pNMHDR);
    if (kd->wVKey == VK_SPACE)
    {
        std::vector<int> rows;
        POSITION pos = m_list.GetFirstSelectedItemPosition();
        while (pos)
            rows.push_back(m_list.GetNextSelectedItem(pos));
        ToggleViewItems(rows);
    }
    *pResult = 0;
}

void CRecoverAsideDlg::OnCheckAll()
{
    for (int i : m_view)
        m_checked[i] = 1;
    m_list.Invalidate();
    OnSelectionChanged();
}

void CRecoverAsideDlg::OnUncheckAll()
{
    std::fill(m_checked.begin(), m_checked.end(), (char)0);
    m_list.Invalidate();
    OnSelectionChanged();
}

CRecoverAsideDlg::SelStats CRecoverAsideDlg::CollectSelection() const
{
    SelStats st;
    if (m_checked.empty())
        return st;

    // Checking a folder recovers everything that was deleted inside it.
    std::vector<char> inc(m_entries.size(), 0);
    std::vector<int> stack;
    for (int i = 0; i < (int)m_entries.size(); ++i)
    {
        if (!m_checked[i] || inc[i])
            continue;
        stack.push_back(i);
        while (!stack.empty())
        {
            int j = stack.back();
            stack.pop_back();
            if (inc[j])
                continue;
            inc[j] = 1;
            const DeletedEntry& e = m_entries[j];
            if (!e.isDir)
                continue;
            auto it = m_children.find(e.recordNo);
            if (it != m_children.end())
                for (int c : it->second)
                    if (!inc[c] && IsChildOf(m_entries[c], e))
                        stack.push_back(c);
        }
    }

    std::vector<int> items;
    for (int i = 0; i < (int)m_entries.size(); ++i)
        if (inc[i])
            items.push_back(i);
    return StatsFor(items);
}

CRecoverAsideDlg::SelStats CRecoverAsideDlg::StatsFor(const std::vector<int>& items) const
{
    SelStats st;
    for (int i : items)
    {
        const DeletedEntry& e = m_entries[i];
        st.items.push_back(i);
        if (e.isDir) { ++st.dirs; continue; }
        if (e.state == RecoverState::Unsupported) { ++st.unsupported; continue; }
        if (e.state != RecoverState::Good) ++st.partial;
        ++st.files;
        st.bytes += e.size;
        st.largest = max(st.largest, e.size);
    }
    return st;
}

void CRecoverAsideDlg::OnSelectionChanged()
{
    SelStats st = CollectSelection();
    CString s;
    if (st.items.empty())
        s = L"Nothing checked. Tick files or folders to recover (checking a folder includes everything deleted inside it).";
    else
    {
        s.Format(L"Checked: %zu file(s), %zu folder(s), %s of data.", st.files, st.dirs, (LPCWSTR)FormatBytes(st.bytes));
        if (st.partial)
        {
            CString p;
            p.Format(L"  %zu file(s) are partly/fully overwritten and may be damaged.", st.partial);
            s += p;
        }
        if (st.unsupported)
        {
            CString u;
            u.Format(L"  %zu unsupported file(s) will be skipped.", st.unsupported);
            s += u;
        }
    }
    SetDlgItemText(IDC_SELINFO, s);

    int d = SelectedDestVolume();
    FillDestinations(d >= 0 ? m_volumes[d].root : CString());
}

// ---------------------------------------------------------------- recovery

CStringW CRecoverAsideDlg::RelativeOutputPath(const DeletedEntry& e) const
{
    CStringW rel = L"\\" + CStringW(m_scanSrc.root.Left(1)) + L"_drive";
    int pos = 0;
    CStringW tok = e.path.Tokenize(L"\\", pos);
    while (!tok.IsEmpty())
    {
        rel += L"\\" + SanitizeComponent(tok);
        tok = e.path.Tokenize(L"\\", pos);
    }
    return rel + L"\\" + SanitizeComponent(e.name);
}

bool CRecoverAsideDlg::ConfirmDamagedFiles(SelStats& st)
{
    std::vector<int> damaged;
    std::vector<CDamagedFilesDlg::Row> rows;
    for (int i : st.items)
    {
        const DeletedEntry& e = m_entries[i];
        if (e.isDir || (e.state != RecoverState::Partial && e.state != RecoverState::Overwritten))
            continue;
        bool full = e.state == RecoverState::Overwritten;
        damaged.push_back(i);
        rows.push_back({ e.name, CellText(i, COL_PATH), FormatBytes(e.size), StateText(e), full, !full });
    }
    m_userSkipped.clear();
    if (rows.empty())
        return true;

    CDamagedFilesDlg dlg(rows, this);
    if (dlg.DoModal() != IDOK)
        return false;

    std::vector<char> skip(m_entries.size(), 0);
    for (size_t k = 0; k < rows.size(); ++k)
        if (!rows[k].include)
        {
            skip[damaged[k]] = 1;
            m_userSkipped.push_back(damaged[k]);
        }
    std::vector<int> keep;
    for (int i : st.items)
        if (!skip[i])
            keep.push_back(i);
    st = StatsFor(keep);
    if (st.files == 0 && st.dirs == 0)
    {
        AfxMessageBox(L"All selected files were skipped, so there is nothing left to recover.", MB_ICONINFORMATION);
        return false;
    }
    return true;
}

void CRecoverAsideDlg::OnRecover()
{
    if (m_busy)
        return;
    if (!m_haveScan || !m_ntfs)
    {
        AfxMessageBox(L"Scan a source volume first.", MB_ICONINFORMATION);
        return;
    }
    SelStats st = CollectSelection();
    if (st.files == 0 && st.dirs == 0)
    {
        AfxMessageBox(L"Tick the files or folders you want to recover first.", MB_ICONINFORMATION);
        return;
    }

    // Warn about overwritten files first: skipping some changes how much space is needed.
    if (!ConfirmDamagedFiles(st))
        return;

    // Find a destination that cannot harm the source disk. Loop until one is attached or the user gives up.
    VolumeInfo dst;
    for (;;)
    {
        RefreshVolumes();
        int d = SelectedDestVolume();
        CString reason = d >= 0 ? CheckDestination(m_volumes[d], m_scanSrc, st) : CString(L"no destination selected");
        if (reason.IsEmpty())
        {
            dst = m_volumes[d];
            break;
        }

        int best = BestDestination(m_scanSrc, st);
        if (best >= 0)
        {
            CString msg = L"The selected destination cannot be used:\n" + reason +
                          L".\n\nUse this safe destination instead?\n\n" + m_volumes[best].Describe();
            if (AfxMessageBox(msg, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            dst = m_volumes[best];
            FillDestinations(dst.root);
            break;
        }

        VolumeInfo typical;
        CString disks;
        for (DWORD n : m_scanSrc.disks)
        {
            CString t;
            t.Format(disks.IsEmpty() ? L"%u" : L", %u", n);
            disks += t;
        }
        CString msg;
        msg.Format(L"No safe destination drive is available.\n\n"
                   L"To avoid overwriting the deleted data you are trying to recover, files are never written to "
                   L"the physical disk they came from (%s, Disk %s).\n\n"
                   L"Please attach a USB flash drive or external disk with at least %s free "
                   L"(selected: %zu file(s), %s of data), then click Retry.",
                   (LPCWSTR)m_scanSrc.root.Left(2), (LPCWSTR)disks,
                   (LPCWSTR)FormatBytes(RequiredBytes(typical, st)), st.files, (LPCWSTR)FormatBytes(st.bytes));
        if (st.largest > 0xFFFFFFFFull)
            msg += L"\n\nNote: at least one file is larger than 4 GB, so the drive must be NTFS or exFAT (not FAT32).";
        if (AfxMessageBox(msg, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return;
    }

    CTime now = CTime::GetCurrentTime();
    CStringW destBase = dst.root + L"RecoverAside_" + now.Format(L"%Y%m%d_%H%M%S");

    CString confirm;
    confirm.Format(L"Recover %zu file(s) and %zu folder(s) (%s)\n\nfrom:  %s  (read-only)\nto:      %s\n\nContinue?",
                   st.files, st.dirs, (LPCWSTR)FormatBytes(st.bytes), (LPCWSTR)m_scanSrc.root, (LPCWSTR)destBase);
    if (st.partial)
    {
        CString w;
        w.Format(L"\n\nWarning: %zu of these file(s) were overwritten and will probably be damaged.", st.partial);
        confirm.Insert(confirm.Find(L"\n\nContinue?"), w);
    }
    if (!m_userSkipped.empty())
    {
        CString w;
        w.Format(L"\n%zu damaged file(s) you unticked will be skipped.", m_userSkipped.size());
        confirm.Insert(confirm.Find(L"\n\nContinue?"), w);
    }
    if (AfxMessageBox(confirm, MB_OKCANCEL | (st.partial ? MB_ICONWARNING : MB_ICONQUESTION)) != IDOK)
        return;

    auto* job = new RecoverJob;
    job->hwnd = GetSafeHwnd();
    job->vol = m_ntfs.get();
    job->destBase = destBase;
    job->srcRoot = m_scanSrc.root;
    job->totalBytes = st.bytes;
    job->cancel = &m_cancel;
    for (int i : st.items)
        job->items.push_back({ &m_entries[i], RelativeOutputPath(m_entries[i]) });
    for (int i : m_userSkipped)
        job->userSkipped.push_back({ &m_entries[i], RelativeOutputPath(m_entries[i]) });
    // Folders first so empty deleted folders are recreated too.
    std::stable_partition(job->items.begin(), job->items.end(), [](const RecoverItem& r) { return r.e->isDir; });

    m_cancel = false;
    SetBusy(true, L"Recovering to " + destBase + L" ...");
    if (!AfxBeginThread(RecoverThread, job))
    {
        delete job;
        SetBusy(false, L"Could not start the recovery thread.");
    }
}

LRESULT CRecoverAsideDlg::OnRecoverProgress(WPARAM wParam, LPARAM)
{
    m_progress.SetPos((int)wParam);
    CString s;
    s.Format(L"Recovering... %d%%", (int)wParam / 10);
    SetStatus(s);
    return 0;
}

LRESULT CRecoverAsideDlg::OnRecoverDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<RecoverResult> res(reinterpret_cast<RecoverResult*>(lParam));
    SetBusy(false, L"");
    if (m_closePending)
    {
        EndDialog(IDCANCEL);
        return 0;
    }
    if (!res->fatal.IsEmpty())
    {
        SetStatus(res->fatal);
        AfxMessageBox(res->fatal, MB_ICONERROR);
        return 0;
    }

    m_progress.SetPos(1000);
    CString s;
    s.Format(L"%s%d file(s) recovered OK, %d with warnings, %d failed, %d skipped, %d folder(s) created (%s).",
             res->cancelled ? L"Cancelled. " : L"", res->ok, res->warned, res->failed, res->skipped, res->dirs,
             (LPCWSTR)FormatBytes(res->bytes));
    SetStatus(s);
    RefreshVolumes();

    if (AfxMessageBox(s + L"\n\nDetails are in RecoveryLog.txt.\n\nOpen the recovery folder now?", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(GetSafeHwnd(), L"open", res->destBase, nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

// ---------------------------------------------------------------- misc

void CRecoverAsideDlg::SetBusy(bool busy, const CString& status)
{
    m_busy = busy;
    const UINT ids[] = { IDC_SCAN, IDC_REFRESH, IDC_RECOVER, IDC_SOURCE, IDC_DEST, IDC_CHECKALL, IDC_UNCHECKALL };
    for (UINT id : ids)
        GetDlgItem(id)->EnableWindow(!busy);
    GetDlgItem(IDC_STOP)->EnableWindow(busy);
    if (busy)
        m_progress.SetPos(0);
    if (!status.IsEmpty())
        SetStatus(status);
}

void CRecoverAsideDlg::OnStop()
{
    m_cancel = true;
    SetStatus(L"Stopping...");
}

void CRecoverAsideDlg::OnCancel()
{
    if (m_busy)
    {
        if (AfxMessageBox(L"An operation is still running. Stop it and close?", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            m_cancel = true;
            m_closePending = true;
        }
        return;
    }
    CDialogEx::OnCancel();
}

void CRecoverAsideDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    if (m_anchors.empty() || nType == SIZE_MINIMIZED)
        return;
    int dx = cx - m_initClient.Width();
    int dy = cy - m_initClient.Height();
    HDWP hdwp = BeginDeferWindowPos((int)m_anchors.size());
    for (const Anchor& a : m_anchors)
    {
        CRect r = a.rc;
        if (a.flags & A_MOVEX) r.OffsetRect(dx, 0);
        if (a.flags & A_MOVEY) r.OffsetRect(0, dy);
        if (a.flags & A_SIZEX) r.right += dx;
        if (a.flags & A_SIZEY) r.bottom += dy;
        if (hdwp)
            hdwp = DeferWindowPos(hdwp, GetDlgItem(a.id)->GetSafeHwnd(), nullptr, r.left, r.top, r.Width(), r.Height(),
                                  SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hdwp)
        EndDeferWindowPos(hdwp);
    Invalidate();
}

void CRecoverAsideDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    if (!m_initWindow.IsRectEmpty())
    {
        lpMMI->ptMinTrackSize.x = m_initWindow.Width();
        lpMMI->ptMinTrackSize.y = m_initWindow.Height();
    }
    CDialogEx::OnGetMinMaxInfo(lpMMI);
}
