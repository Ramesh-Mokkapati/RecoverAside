// DamagedFilesDlg.h : warns about overwritten files before recovery and lets the user
// choose which of them to recover anyway.
#pragma once

class CDamagedFilesDlg : public CDialogEx
{
public:
    struct Row
    {
        CString name;
        CString location;
        CString size;
        CString status;
        bool    fullyOverwritten;
        bool    include;            // in: default choice, out: user's choice
    };

    CDamagedFilesDlg(std::vector<Row>& rows, CWnd* pParent = nullptr);
    enum { IDD = IDD_DAMAGED_FILES };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;

    afx_msg void OnIncludePartial();
    afx_msg void OnIncludeAll();
    afx_msg void OnSkipAll();
    afx_msg void OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    DECLARE_MESSAGE_MAP()

private:
    void SetChecks(bool partial, bool full);
    void UpdateCount();

    std::vector<Row>& m_rows;
    CListCtrl m_list;
    bool m_ready = false;
};
