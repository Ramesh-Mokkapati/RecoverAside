// RecoverAside.cpp : application entry
#include "stdafx.h"
#include "RecoverAside.h"
#include "RecoverAsideDlg.h"

BEGIN_MESSAGE_MAP(CRecoverAsideApp, CWinApp)
END_MESSAGE_MAP()

CRecoverAsideApp theApp;

BOOL CRecoverAsideApp::InitInstance()
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

    CRecoverAsideDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}
