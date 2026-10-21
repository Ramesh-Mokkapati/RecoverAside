// RecoverAsideDlg.h : main dialog
#pragma once

#include "DriveInfo.h"
#include "NtfsVolume.h"

class CRecoverAsideDlg : public CDialogEx
{
public:
    explicit CRecoverAsideDlg(CWnd* pParent = nullptr);
    enum { IDD = IDD_RECOVERASIDE_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}     // Enter must not close the window
    void OnCancel() override;

    afx_msg void OnScan();
    afx_msg void OnRefresh();
    afx_msg void OnRecover();
    afx_msg void OnStop();
    afx_msg void OnCheckAll();
    afx_msg void OnUncheckAll();
    afx_msg void OnFilterChange();
    afx_msg void OnSourceChange();
    afx_msg void OnDestChange();
    afx_msg void OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnListClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnListKeyDown(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnColumnClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnListDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTreeExpanding(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTreeSelChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnBrowseFolder();
    afx_msg void OnScopeOptionChange();
    afx_msg void OnShowDeletedDirsChange();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
    afx_msg BOOL OnDeviceChange(UINT nEventType, DWORD_PTR dwData);
    afx_msg LRESULT OnScanProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnScanDone(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnRecoverProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnRecoverDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    struct SelStats
    {
        size_t files = 0, dirs = 0, partial = 0, unsupported = 0;
        ULONGLONG bytes = 0, largest = 0;
        std::vector<int> items;     // checked entries plus contents of checked folders
    };
    enum class FolderState { Live, Deleted, Virtual };
    struct FolderNode
    {
        wchar_t     letter = 0;     // drive letter, upper case
        CStringW    name;
        CStringW    path;           // relative to the volume root; L"" for the drive itself
        FolderState state = FolderState::Virtual;
        int         parent = -1;    // -1 for a drive
        std::vector<int> children;
        size_t      deletedFiles = 0;   // deleted files in this folder and below (scanned drive only)
        HTREEITEM   item = nullptr;
        bool        populated = false;  // tree items inserted for children
        bool        enumerated = false; // existing subfolders listed from the file system
    };
    struct Anchor
    {
        UINT  id;
        CRect rc;
        int   flags;
    };

    // drives
    void RefreshVolumes();
    void FillDestinations(const CString& preferRoot);
    void UpdateDestInfo(const SelStats& st);
    bool GetCheckSource(VolumeInfo& out) const;
    int  SelectedDestVolume() const;
    int  BestDestination(const VolumeInfo& src, const SelStats& st) const;
    static ULONGLONG RequiredBytes(const VolumeInfo& dst, const SelStats& st);
    static CString CheckDestination(const VolumeInfo& dst, const VolumeInfo& src, const SelStats& st);

    // results
    void BuildChildren();
    void ComputeFolderSizes();
    void RebuildView();
    void SortView();
    void ToggleViewItems(const std::vector<int>& viewRows);
    void OnSelectionChanged();
    SelStats CollectSelection() const;
    SelStats StatsFor(const std::vector<int>& items) const;
    bool ConfirmDamagedFiles(SelStats& st);
    CString CellText(int idx, int col) const;
    CStringW RelativeOutputPath(const DeletedEntry& e) const;
    bool IsChildOf(const DeletedEntry& child, const DeletedEntry& dir) const;
    bool InScope(const DeletedEntry& e, bool subfolders) const;

    // folder tree / scope
    wchar_t ScanLetter() const;
    const VolumeInfo* VolumeFor(wchar_t letter) const;
    int  RootNodeFor(wchar_t letter) const;
    int  FolderNodeFor(wchar_t letter, const CStringW& path);
    void EnumerateLive(int node);
    void ApplyScanOverlay();
    void ResetTree();
    void InsertNode(HTREEITEM parentItem, int node);
    void PopulateChildren(int node);
    bool IsNodeVisible(int node) const;
    CString NodeLabel(int node) const;
    void SetScope(const CStringW& path);
    void SelectTreePath(wchar_t letter, const CStringW& path);
    void OnFolderChosen(wchar_t letter, const CStringW& path);
    void StartScan(wchar_t letter, bool automatic);
    afx_msg LRESULT OnStartScan(WPARAM wParam, LPARAM lParam);

    void SetBusy(bool busy, const CString& status);
    void SetStatus(const CString& s) { SetDlgItemText(IDC_STATUS, s); }

    CListCtrl     m_list;
    CTreeCtrl     m_tree;
    CImageList    m_treeImages;
    CComboBox     m_cbSrc, m_cbDst;
    CProgressCtrl m_progress;
    HICON         m_hIcon = nullptr;

    std::vector<VolumeInfo> m_volumes;
    std::vector<int> m_srcMap, m_dstMap;    // combo index -> m_volumes index

    std::unique_ptr<NtfsVolume> m_ntfs;
    VolumeInfo m_scanSrc, m_pendingSrc;
    bool m_haveScan = false;
    std::vector<DeletedEntry> m_entries;
    std::vector<char> m_checked;
    std::vector<size_t> m_folderItems;
    std::unordered_map<ULONGLONG, std::vector<int>> m_children;   // parent record -> entries
    std::vector<int> m_view;
    std::vector<int> m_userSkipped;     // damaged files the user chose not to recover
    std::vector<FolderInfo> m_scanFolders;                  // folders reported by the last scan
    std::vector<FolderNode> m_folders;                      // drives first, then folders
    std::unordered_map<std::wstring, int> m_folderIndex;    // letter + lower-case path -> node
    wchar_t  m_scopeLetter = 0;                             // drive of the selected folder
    CStringW m_scope;                                       // folder whose deleted items are listed
    CString  m_systemScanOk;                                // drives whose system-volume warning was accepted
    CString  m_declinedScan;                                // drives the user chose not to auto-scan
    bool m_suppressTreeSel = false;
    int  m_sortCol = 0;
    bool m_sortAsc = true;

    std::atomic<bool> m_cancel{ false };
    bool m_busy = false;
    bool m_closePending = false;

    std::vector<Anchor> m_anchors;
    CRect m_initClient, m_initWindow;
};
