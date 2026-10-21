// stdafx.h : precompiled header for RecoverAside
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#include <SDKDDKVer.h>

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#include <afxwin.h>
#include <afxext.h>
#include <afxdialogex.h>
#include <afxcmn.h>

#include <winioctl.h>
#include <dbt.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <memory>
#include <algorithm>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
