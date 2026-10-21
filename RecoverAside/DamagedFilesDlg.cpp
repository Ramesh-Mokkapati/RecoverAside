#include "stdafx.h"
#include "resource.h"
#include "DamagedFilesDlg.h"

CDamagedFilesDlg::CDamagedFilesDlg(std::vector<Row>& rows, CWnd* pParent)
    : CDialogEx(IDD_DAMAGED_FILES, pParent), m_rows(rows)
{
}

void CDamagedFilesDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_DMG_LIST, m_list);
}

BEGIN_MESSAGE_MAP(CDamagedFilesDlg, CDialogEx)
    ON_BN_CLICKED(IDC_DMG_INCLUDEPARTIAL, &CDamagedFilesDlg::OnIncludePartial)
    ON_BN_CLICKED(IDC_DMG_INCLUDEALL, &CDamagedFilesDlg::OnIncludeAll)
    ON_BN_CLICKED(IDC_DMG_SKIPALL, &CDamagedFilesDlg::OnSkipAll)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_DMG_LIST, &CDamagedFilesDlg::OnItemChanged)
END_MESSAGE_MAP()

BOOL CDamagedFilesDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    size_t full = 0;
    for (const Row& r : m_rows)
        if (r.fullyOverwritten)
            ++full;
    CString text;
    text.Format(L"%zu of the files you selected were partly or fully overwritten after they were deleted "
                L"(%zu partly, %zu fully). Recovered copies of these files will probably be DAMAGED: "
                L"they may not open, or may contain pieces of other files.\n\n"
                L"Fully overwritten files hold none of their original data, so they are unticked by default. "
                L"Tick the files you still want to recover; unticked files are skipped.",
                m_rows.size(), m_rows.size() - full, full);
    SetDlgItemText(IDC_DMG_TEXT, text);

    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    m_list.InsertColumn(0, L"Name", LVCFMT_LEFT, 170);
    m_list.InsertColumn(1, L"Original location", LVCFMT_LEFT, 200);
    m_list.InsertColumn(2, L"Size", LVCFMT_RIGHT, 70);
    m_list.InsertColumn(3, L"Status", LVCFMT_LEFT, 170);
    m_list.SetItemCount((int)m_rows.size());
    for (int i = 0; i < (int)m_rows.size(); ++i)
    {
        const Row& r = m_rows[i];
        m_list.InsertItem(i, r.name);
        m_list.SetItemText(i, 1, r.location);
        m_list.SetItemText(i, 2, r.size);
        m_list.SetItemText(i, 3, r.status);
        m_list.SetCheck(i, r.include);
    }
    m_ready = true;
    UpdateCount();
    MessageBeep(MB_ICONWARNING);
    return TRUE;
}

void CDamagedFilesDlg::SetChecks(bool partial, bool full)
{
    m_ready = false;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        m_list.SetCheck(i, m_rows[i].fullyOverwritten ? full : partial);
    m_ready = true;
    UpdateCount();
}

void CDamagedFilesDlg::OnIncludePartial() { SetChecks(true, false); }
void CDamagedFilesDlg::OnIncludeAll()     { SetChecks(true, true); }
void CDamagedFilesDlg::OnSkipAll()        { SetChecks(false, false); }

void CDamagedFilesDlg::OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* lv = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    if (m_ready && (lv->uChanged & LVIF_STATE) &&
        ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
        UpdateCount();
    *pResult = 0;
}

void CDamagedFilesDlg::UpdateCount()
{
    int n = 0;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (m_list.GetCheck(i))
            ++n;
    CString s;
    s.Format(L"%d of %zu damaged file(s) will be recovered; %zu will be skipped.",
             n, m_rows.size(), m_rows.size() - n);
    SetDlgItemText(IDC_DMG_COUNT, s);
}

void CDamagedFilesDlg::OnOK()
{
    for (int i = 0; i < (int)m_rows.size(); ++i)
        m_rows[i].include = m_list.GetCheck(i) != FALSE;
    CDialogEx::OnOK();
}
