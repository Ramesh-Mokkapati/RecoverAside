// DriveInfo.h : volume enumeration and physical-disk mapping used to keep
// recovered data off the disk being recovered.
#pragma once

struct VolumeInfo
{
    CString   root;                 // L"D:\\"
    CString   device;               // L"\\\\.\\D:"
    CString   label;
    CString   fsName;               // NTFS, FAT32, exFAT ...
    UINT      driveType = DRIVE_UNKNOWN;
    ULONGLONG totalBytes = 0;
    ULONGLONG freeBytes = 0;
    DWORD     bytesPerCluster = 4096;
    std::vector<DWORD> disks;       // physical disk numbers backing this volume
    bool      disksKnown = false;
    bool      readOnly = false;
    bool      isSystem = false;     // holds the Windows directory
    bool      isUsb = false;        // USB / SD / MMC bus

    bool IsNtfs() const  { return fsName.CompareNoCase(L"NTFS") == 0; }
    bool IsFat32() const { return fsName.CompareNoCase(L"FAT32") == 0 || fsName.CompareNoCase(L"FAT") == 0; }
    ULONGLONG UsedBytes() const { return totalBytes > freeBytes ? totalBytes - freeBytes : 0; }
    CString Describe() const;
};

// Fixed and removable volumes that are mounted and formatted.
std::vector<VolumeInfo> EnumerateVolumes();

// True when the two volumes live (even partly) on the same physical disk.
bool SharePhysicalDisk(const VolumeInfo& a, const VolumeInfo& b);

CString FormatBytes(ULONGLONG bytes);
