// RecoverAside.h : application class
#pragma once

#include "resource.h"

class CRecoverAsideApp : public CWinApp
{
public:
    BOOL InitInstance() override;
    DECLARE_MESSAGE_MAP()
};

extern CRecoverAsideApp theApp;
