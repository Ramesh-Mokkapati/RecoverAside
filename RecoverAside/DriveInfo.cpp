#include "stdafx.h"
#include "DriveInfo.h"

CString FormatBytes(ULONGLONG bytes)
{
    static const wchar_t* units[] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    CString s;
    if (u == 0) s.Format(L"%llu bytes", bytes);
    else        s.Format(L"%.1f %s", v, units[u]);
    return s;
}

static bool QueryDiskNumbers(const CString& device, std::vector<DWORD>& disks)
{
    // Zero access rights: we only need metadata, never data.
    HANDLE h = CreateFileW(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::vector<BYTE> buf(sizeof(VOLUME_DISK_EXTENTS) + 64 * sizeof(DISK_EXTENT));
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                              buf.data(), (DWORD)buf.size(), &ret, nullptr);
    CloseHandle(h);
    if (!ok)
        return false;

    auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
    for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i)
    {
        DWORD n = ext->Extents[i].DiskNumber;
        if (std::find(disks.begin(), disks.end(), n) == disks.end())
            disks.push_back(n);
    }
    return !disks.empty();
}

static bool IsUsbDisk(DWORD diskNumber)
{
    CString path;
    path.Format(L"\\\\.\\PhysicalDrive%u", diskNumber);
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD ret = 0;
    bool usb = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr))
    {
        auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
        usb = d->BusType == BusTypeUsb || d->BusType == BusTypeSd || d->BusType == BusTypeMmc;
    }
    CloseHandle(h);
    return usb;
}

std::vector<VolumeInfo> EnumerateVolumes()
{
    std::vector<VolumeInfo> out;
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(mask & (1u << i)))
            continue;

        VolumeInfo v;
        wchar_t letter = (wchar_t)(L'A' + i);
        v.root.Format(L"%c:\\", letter);
        v.device.Format(L"\\\\.\\%c:", letter);
        v.driveType = GetDriveTypeW(v.root);
        if (v.driveType != DRIVE_FIXED && v.driveType != DRIVE_REMOVABLE)
            continue;   // skip optical, network and RAM disks

        // Avoid "No disk in drive" popups for empty card readers.
        UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
        wchar_t label[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};
        DWORD flags = 0;
        BOOL haveInfo = GetVolumeInformationW(v.root, label, MAX_PATH + 1, nullptr, nullptr, &flags, fs, MAX_PATH + 1);
        SetErrorMode(oldMode);
        if (!haveInfo)
            continue;

        v.label = label;
        v.fsName = fs;
        v.readOnly = (flags & FILE_READ_ONLY_VOLUME) != 0;

        ULARGE_INTEGER freeCaller = {}, total = {}, freeTotal = {};
        if (GetDiskFreeSpaceExW(v.root, &freeCaller, &total, &freeTotal))
        {
            v.totalBytes = total.QuadPart;
            v.freeBytes = freeCaller.QuadPart;
        }
        DWORD spc = 0, bps = 0, fc = 0, tc = 0;
        if (GetDiskFreeSpaceW(v.root, &spc, &bps, &fc, &tc) && spc && bps)
            v.bytesPerCluster = spc * bps;

        v.isSystem = towupper(winDir[0]) == letter;
        v.disksKnown = QueryDiskNumbers(v.device, v.disks);
        if (v.disksKnown)
            v.isUsb = IsUsbDisk(v.disks[0]);

        out.push_back(v);
    }
    return out;
}

bool SharePhysicalDisk(const VolumeInfo& a, const VolumeInfo& b)
{
    for (DWORD d : a.disks)
        if (std::find(b.disks.begin(), b.disks.end(), d) != b.disks.end())
            return true;
    return false;
}

CString VolumeInfo::Describe() const
{
    CString s = root;
    if (!label.IsEmpty())
        s += L"  [" + label + L"]";
    s += L"  " + fsName;
    s += L"  -  " + FormatBytes(freeBytes) + L" free of " + FormatBytes(totalBytes);

    CString disk;
    if (disksKnown)
    {
        disk = L"  -  Disk ";
        for (size_t i = 0; i < disks.size(); ++i)
        {
            CString n;
            n.Format(i ? L"+%u" : L"%u", disks[i]);
            disk += n;
        }
    }
    s += disk;
    if (isUsb)                         s += L" (USB)";
    else if (driveType == DRIVE_REMOVABLE) s += L" (Removable)";
    if (isSystem)                      s += L" (System)";
    if (readOnly)                      s += L" (Read-only)";
    return s;
}
