#include "stdafx.h"
#include "NtfsVolume.h"

namespace
{
#pragma pack(push, 1)
struct FILE_RECORD_HEADER
{
    DWORD     Magic;            // "FILE"
    WORD      UsaOffset;
    WORD      UsaCount;
    ULONGLONG Lsn;
    WORD      SequenceNumber;
    WORD      LinkCount;
    WORD      AttributeOffset;
    WORD      Flags;            // 0x01 in use, 0x02 directory
    DWORD     BytesInUse;
    DWORD     BytesAllocated;
    ULONGLONG BaseFileRecord;
    WORD      NextAttributeId;
    WORD      Align;
    DWORD     MftRecordNumber;
};

struct ATTR_HEADER
{
    DWORD Type;
    DWORD Length;
    BYTE  NonResident;
    BYTE  NameLength;
    WORD  NameOffset;
    WORD  Flags;                // 0x0001 compressed, 0x4000 encrypted, 0x8000 sparse
    WORD  AttributeId;
};

struct RESIDENT_ATTR
{
    ATTR_HEADER h;
    DWORD ValueLength;
    WORD  ValueOffset;
    BYTE  IndexedFlag;
    BYTE  Padding;
};

struct NONRESIDENT_ATTR
{
    ATTR_HEADER h;
    ULONGLONG LowestVcn;
    ULONGLONG HighestVcn;
    WORD      MappingPairsOffset;
    WORD      CompressionUnit;
    DWORD     Padding;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    ULONGLONG InitializedSize;
};

struct STD_INFO_ATTR
{
    ULONGLONG CreationTime;
    ULONGLONG ModifiedTime;
    ULONGLONG MftChangeTime;
    ULONGLONG AccessTime;
    DWORD     FileAttributes;
};

struct FILE_NAME_ATTR
{
    ULONGLONG ParentRef;
    ULONGLONG CreationTime;
    ULONGLONG ModifiedTime;
    ULONGLONG MftChangeTime;
    ULONGLONG AccessTime;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    DWORD     FileAttributes;
    DWORD     Reparse;
    BYTE      NameLength;
    BYTE      NameType;         // 0 POSIX, 1 Win32, 2 DOS, 3 Win32+DOS
    WCHAR     Name[1];
};
#pragma pack(pop)

const DWORD kFileMagic        = 0x454C4946;   // "FILE"
const DWORD kAttrStdInfo      = 0x10;
const DWORD kAttrAttrList     = 0x20;
const DWORD kAttrFileName     = 0x30;
const DWORD kAttrData         = 0x80;
const DWORD kAttrEnd          = 0xFFFFFFFF;
const ULONGLONG kRootRecord   = 5;
const ULONGLONG kFirstUserRec = 24;          // 0-15 system files, 16-23 reserved
const DWORD kChunkBytes       = 4 * 1024 * 1024;
const ULONGLONG kRefMask      = 0x0000FFFFFFFFFFFFull;

FILETIME ToFileTime(ULONGLONG t)
{
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    return ft;
}

// Replace the update-sequence markers at the end of each 512-byte stride.
bool ApplyFixup(BYTE* rec, DWORD size)
{
    auto* h = reinterpret_cast<FILE_RECORD_HEADER*>(rec);
    DWORD usaOfs = h->UsaOffset, usaCount = h->UsaCount;
    if (usaCount < 2 || usaOfs + usaCount * 2 > size || (usaCount - 1) * 512 > size)
        return false;
    WORD* usa = reinterpret_cast<WORD*>(rec + usaOfs);
    for (DWORD i = 1; i < usaCount; ++i)
    {
        WORD* tail = reinterpret_cast<WORD*>(rec + i * 512 - 2);
        if (*tail != usa[0])
            return false;   // torn / corrupt record
        *tail = usa[i];
    }
    return true;
}

bool DecodeRuns(const BYTE* p, const BYTE* end, std::vector<DataRun>& runs)
{
    LONGLONG lcn = 0;
    while (p < end && *p)
    {
        BYTE hdr = *p++;
        int lenSize = hdr & 0x0F, offSize = hdr >> 4;
        if (lenSize == 0 || lenSize > 8 || offSize > 8 || p + lenSize + offSize > end)
            return false;

        ULONGLONG len = 0;
        for (int i = 0; i < lenSize; ++i)
            len |= (ULONGLONG)p[i] << (8 * i);
        p += lenSize;

        if (offSize == 0)
        {
            runs.push_back({ -1, len });
            continue;
        }
        ULONGLONG off = 0;
        for (int i = 0; i < offSize; ++i)
            off |= (ULONGLONG)p[i] << (8 * i);
        if (offSize < 8 && (p[offSize - 1] & 0x80))
            off |= ~0ull << (8 * offSize);   // sign-extend
        p += offSize;
        lcn += (LONGLONG)off;
        runs.push_back({ lcn, len });
    }
    return true;
}
} // namespace

struct NtfsVolume::ParsedRecord
{
    bool      haveName = false;
    int       nameRank = -1;
    CStringW  name;
    ULONGLONG parentRef = 0;

    bool      haveStd = false;
    STD_INFO_ATTR std = {};

    bool      haveData = false;
    bool      dataResident = false;
    WORD      dataFlags = 0;
    ULONGLONG dataSize = 0;
    ULONGLONG initSize = 0;
    ULONGLONG highestVcn = 0;
    bool      runsOk = true;
    std::vector<BYTE>    resData;
    std::vector<DataRun> runs;

    bool      hasAttrList = false;
};

bool NtfsVolume::Open(const CString& device, CString& err)
{
    Close();
    // GENERIC_READ only. The source volume is never opened for writing.
    m_h = CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_h == INVALID_HANDLE_VALUE)
    {
        err.Format(L"Cannot open %s for reading (error %u). Run the program as Administrator.", (LPCWSTR)device, GetLastError());
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER vd = {};
    DWORD ret = 0;
    if (!DeviceIoControl(m_h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof(vd), &ret, nullptr))
    {
        err.Format(L"%s is not an NTFS volume (error %u).", (LPCWSTR)device, GetLastError());
        Close();
        return false;
    }
    m_bpc = vd.BytesPerCluster;
    m_bps = vd.BytesPerSector;
    m_recSize = vd.BytesPerFileRecordSegment;
    m_totalClusters = (ULONGLONG)vd.TotalClusters.QuadPart;
    m_mftStartLcn = vd.MftStartLcn.QuadPart;
    m_mftValidLen = (ULONGLONG)vd.MftValidDataLength.QuadPart;
    if (!m_bpc || !m_recSize || m_recSize % 512 || kChunkBytes % m_bpc)
    {
        err = L"Unsupported NTFS geometry.";
        Close();
        return false;
    }
    return true;
}

void NtfsVolume::Close()
{
    if (m_h != INVALID_HANDLE_VALUE)
        CloseHandle(m_h);
    m_h = INVALID_HANDLE_VALUE;
    m_bitmap.clear();
    m_dirs.clear();
}

bool NtfsVolume::ReadBytes(ULONGLONG offset, void* buf, DWORD len)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(m_h, li, nullptr, FILE_BEGIN))
        return false;
    DWORD got = 0;
    return ReadFile(m_h, buf, len, &got, nullptr) && got == len;
}

bool NtfsVolume::LoadBitmap(CString& err)
{
    STARTING_LCN_INPUT_BUFFER in = {};
    size_t size = sizeof(VOLUME_BITMAP_BUFFER) + (size_t)(m_totalClusters / 8) + 16;
    std::vector<BYTE> buf(size);
    DWORD ret = 0;
    if (!DeviceIoControl(m_h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in), buf.data(), (DWORD)size, &ret, nullptr))
    {
        err.Format(L"Cannot read the volume cluster bitmap (error %u).", GetLastError());
        return false;
    }
    auto* vb = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(buf.data());
    size_t bytes = (size_t)((vb->BitmapSize.QuadPart + 7) / 8);
    m_bitmap.assign(vb->Buffer, vb->Buffer + bytes);
    return true;
}

bool NtfsVolume::IsAllocated(ULONGLONG lcn) const
{
    if (lcn >= m_totalClusters || (lcn >> 3) >= m_bitmap.size())
        return true;
    return (m_bitmap[(size_t)(lcn >> 3)] & (1 << (lcn & 7))) != 0;
}

bool NtfsVolume::ParseRecord(BYTE* rec, ParsedRecord& pr, bool wantData) const
{
    auto* h = reinterpret_cast<FILE_RECORD_HEADER*>(rec);
    DWORD limit = min(h->BytesInUse, m_recSize);
    DWORD ofs = h->AttributeOffset;

    while (ofs + sizeof(ATTR_HEADER) <= limit)
    {
        auto* a = reinterpret_cast<ATTR_HEADER*>(rec + ofs);
        if (a->Type == kAttrEnd || a->Length < sizeof(ATTR_HEADER) || ofs + a->Length > limit)
            break;
        BYTE* attrEnd = rec + ofs + a->Length;

        if (!a->NonResident && a->Length >= sizeof(RESIDENT_ATTR))
        {
            auto* r = reinterpret_cast<RESIDENT_ATTR*>(a);
            BYTE* val = (BYTE*)a + r->ValueOffset;
            bool fits = val + r->ValueLength <= attrEnd;

            if (a->Type == kAttrStdInfo && fits && r->ValueLength >= sizeof(STD_INFO_ATTR))
            {
                pr.std = *reinterpret_cast<STD_INFO_ATTR*>(val);
                pr.haveStd = true;
            }
            else if (a->Type == kAttrFileName && fits && r->ValueLength >= offsetof(FILE_NAME_ATTR, Name))
            {
                auto* fn = reinterpret_cast<FILE_NAME_ATTR*>(val);
                if (offsetof(FILE_NAME_ATTR, Name) + fn->NameLength * sizeof(WCHAR) <= r->ValueLength)
                {
                    int rank = (fn->NameType == 1 || fn->NameType == 3) ? 3 : (fn->NameType == 0 ? 2 : 1);
                    if (rank > pr.nameRank)
                    {
                        pr.nameRank = rank;
                        pr.name = CStringW(fn->Name, fn->NameLength);
                        pr.parentRef = fn->ParentRef;
                        pr.haveName = true;
                    }
                }
            }
            else if (a->Type == kAttrAttrList)
            {
                pr.hasAttrList = true;
            }
            else if (a->Type == kAttrData && a->NameLength == 0 && wantData && fits && !pr.haveData)
            {
                pr.haveData = true;
                pr.dataResident = true;
                pr.dataFlags = a->Flags;
                pr.dataSize = pr.initSize = r->ValueLength;
                pr.resData.assign(val, val + r->ValueLength);
            }
        }
        else if (a->NonResident && a->Length >= sizeof(NONRESIDENT_ATTR))
        {
            auto* n = reinterpret_cast<NONRESIDENT_ATTR*>(a);
            if (a->Type == kAttrAttrList)
            {
                pr.hasAttrList = true;
            }
            else if (a->Type == kAttrData && a->NameLength == 0 && n->LowestVcn == 0 && wantData && !pr.haveData)
            {
                pr.haveData = true;
                pr.dataResident = false;
                pr.dataFlags = a->Flags;
                pr.dataSize = n->DataSize;
                pr.initSize = n->InitializedSize;
                pr.highestVcn = n->HighestVcn;
                pr.runsOk = DecodeRuns((BYTE*)a + n->MappingPairsOffset, attrEnd, pr.runs);
            }
        }
        ofs += a->Length;
    }
    return true;
}

void NtfsVolume::ProcessRecord(BYTE* rec, ULONGLONG recNo, std::vector<DeletedEntry>& out)
{
    auto* h = reinterpret_cast<FILE_RECORD_HEADER*>(rec);
    if (h->Magic != kFileMagic || !ApplyFixup(rec, m_recSize))
        return;
    if (h->BaseFileRecord & kRefMask)
    {
        // Extension record. Files with many attributes (e.g. OneDrive folders) keep their long
        // Win32 name here while the base record only holds the 8.3 name; remember the best one.
        ParsedRecord pr;
        ParseRecord(rec, pr, false);
        if (pr.haveName)
        {
            ExtName& x = m_extNames[h->BaseFileRecord & kRefMask];
            if (pr.nameRank > x.rank)
                x = { (WORD)(h->BaseFileRecord >> 48), pr.nameRank, pr.name, pr.parentRef };
        }
        return;
    }

    bool inUse = (h->Flags & 0x01) != 0;
    bool isDir = (h->Flags & 0x02) != 0;
    if (inUse && !isDir)
        return;     // live files are irrelevant; live folders are needed for paths

    bool deletedFile = !inUse && recNo >= kFirstUserRec;
    ParsedRecord pr;
    ParseRecord(rec, pr, deletedFile && !isDir);
    if (!pr.haveName && !pr.hasAttrList)
        return;     // no name here and none can be in an extension record

    if (isDir)
        m_dirs[recNo] = { pr.parentRef, pr.name, h->SequenceNumber, inUse, pr.nameRank };

    if (!deletedFile)
        return;

    DeletedEntry e;
    e.recordNo = recNo;
    e.seq = h->SequenceNumber;
    e.parentRecord = pr.parentRef & kRefMask;
    e.parentSeq = (WORD)(pr.parentRef >> 48);
    e.name = pr.name;
    e.isDir = isDir;
    if (pr.haveStd)
    {
        e.created = ToFileTime(pr.std.CreationTime);
        e.modified = ToFileTime(pr.std.ModifiedTime);
        e.accessed = ToFileTime(pr.std.AccessTime);
    }
    Evaluate(e, pr);
    out.push_back(std::move(e));
    m_entryRanks.push_back(pr.nameRank);
}

void NtfsVolume::MergeExtensionNames(std::vector<DeletedEntry>& out)
{
    // The extension record refers to its base with the base's sequence number at the time;
    // a freed base record has since been bumped by one.
    auto seqOk = [](WORD recSeq, WORD refSeq, bool inUse)
    {
        return refSeq == 0 || recSeq == refSeq || (!inUse && recSeq == (WORD)(refSeq + 1));
    };

    for (auto it = m_dirs.begin(); it != m_dirs.end(); )
    {
        DirNode& d = it->second;
        auto x = m_extNames.find(it->first);
        if (x != m_extNames.end() && x->second.rank > d.nameRank && seqOk(d.seq, x->second.baseSeq, d.inUse))
        {
            d.name = x->second.name;
            d.parentRef = x->second.parentRef;
            d.nameRank = x->second.rank;
        }
        it = d.nameRank < 0 ? m_dirs.erase(it) : std::next(it);
    }

    size_t keep = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        DeletedEntry& e = out[i];
        auto x = m_extNames.find(e.recordNo);
        if (x != m_extNames.end() && x->second.rank > m_entryRanks[i] && seqOk(e.seq, x->second.baseSeq, false))
        {
            e.name = x->second.name;
            e.parentRecord = x->second.parentRef & kRefMask;
            e.parentSeq = (WORD)(x->second.parentRef >> 48);
            m_entryRanks[i] = x->second.rank;
        }
        if (m_entryRanks[i] >= 0)
        {
            if (keep != i)
                out[keep] = std::move(e);
            ++keep;
        }
    }
    out.resize(keep);
    m_extNames.clear();
    m_entryRanks.clear();
}

void NtfsVolume::Evaluate(DeletedEntry& e, const ParsedRecord& pr) const
{
    if (e.isDir)
        return;

    if (!pr.haveData)
    {
        e.state = RecoverState::Unsupported;
        e.note = pr.hasAttrList ? L"Data map stored in extension record" : L"No data stream";
        return;
    }
    e.size = pr.dataSize;
    e.initSize = min(pr.initSize, pr.dataSize);

    if (pr.dataFlags & 0x4000) { e.state = RecoverState::Unsupported; e.note = L"EFS encrypted"; return; }
    if (pr.dataFlags & 0x00FF) { e.state = RecoverState::Unsupported; e.note = L"NTFS compressed"; return; }

    if (pr.dataResident)
    {
        e.resident = true;
        e.residentData = pr.resData;
        return;
    }

    e.runs = pr.runs;
    if (!pr.runsOk)
    {
        e.state = RecoverState::Overwritten;
        e.note = L"Corrupt cluster map";
        return;
    }

    ULONGLONG needed = (e.size + m_bpc - 1) / m_bpc;
    ULONGLONG mapped = 0, checked = 0, taken = 0;
    for (const DataRun& r : e.runs)
    {
        if (mapped >= needed)
            break;
        ULONGLONG n = min(r.clusters, needed - mapped);
        mapped += n;
        if (r.lcn < 0)
            continue;   // sparse: zeros, nothing to overwrite
        if ((ULONGLONG)r.lcn + n > m_totalClusters)
        {
            e.state = RecoverState::Overwritten;
            e.note = L"Cluster map points outside volume";
            return;
        }
        // Clusters that are allocated *now* were reused by another file after deletion.
        for (ULONGLONG c = 0; c < n; ++c)
        {
            ULONGLONG lcn = (ULONGLONG)r.lcn + c;
            if ((lcn & 7) == 0 && n - c >= 8)
            {
                BYTE b = m_bitmap[(size_t)(lcn >> 3)];
                if (b == 0x00 || b == 0xFF)
                {
                    if (b) taken += 8;
                    checked += 8;
                    c += 7;
                    continue;
                }
            }
            if (IsAllocated(lcn)) ++taken;
            ++checked;
        }
    }

    if (mapped < needed)
    {
        e.state = RecoverState::Partial;
        e.note = L"Fragment list incomplete (extension record)";
    }
    if (checked)
    {
        e.overwrittenPct = (int)(taken * 100 / checked);
        if (taken == checked)
            e.state = RecoverState::Overwritten;
        else if (taken)
            e.state = RecoverState::Partial;
    }
}

bool NtfsVolume::ParentMatches(ULONGLONG parentRec, WORD parentSeq) const
{
    auto it = m_dirs.find(parentRec);
    if (it == m_dirs.end())
        return false;
    WORD s = it->second.seq;
    // NTFS bumps the sequence number when a record is freed.
    return parentSeq == 0 || s == parentSeq || (!it->second.inUse && s == (WORD)(parentSeq + 1));
}

CStringW NtfsVolume::DirPath(ULONGLONG rec, std::unordered_map<ULONGLONG, CStringW>& cache, int depth) const
{
    if (rec == kRootRecord)
        return L"";
    auto c = cache.find(rec);
    if (c != cache.end())
        return c->second;

    CStringW result;
    auto it = m_dirs.find(rec);
    if (it == m_dirs.end() || depth > 256)
    {
        result = L"\\$Orphaned";
    }
    else
    {
        ULONGLONG pr = it->second.parentRef & kRefMask;
        WORD ps = (WORD)(it->second.parentRef >> 48);
        CStringW parent = (pr == kRootRecord) ? CStringW()
                        : ParentMatches(pr, ps) ? DirPath(pr, cache, depth + 1)
                        : CStringW(L"\\$Orphaned");
        result = parent + L"\\" + it->second.name;
    }
    cache[rec] = result;
    return result;
}

bool NtfsVolume::Scan(std::vector<DeletedEntry>& out, std::vector<FolderInfo>& folders, const ProgressFn& progress, CString& err)
{
    out.clear();
    folders.clear();
    m_dirs.clear();
    m_extNames.clear();
    m_entryRanks.clear();
    if (m_h == INVALID_HANDLE_VALUE) { err = L"Volume not open."; return false; }
    if (!LoadBitmap(err))
        return false;

    // Record 0 is $MFT itself; its $DATA run list tells us where every other record lives.
    DWORD rec0Len = ((m_recSize + m_bpc - 1) / m_bpc) * m_bpc;
    std::vector<BYTE> rec0(rec0Len);
    if (!ReadBytes((ULONGLONG)m_mftStartLcn * m_bpc, rec0.data(), rec0Len) ||
        reinterpret_cast<FILE_RECORD_HEADER*>(rec0.data())->Magic != kFileMagic ||
        !ApplyFixup(rec0.data(), m_recSize))
    {
        err = L"Cannot read the $MFT record.";
        return false;
    }
    ParsedRecord mft;
    ParseRecord(rec0.data(), mft, true);
    if (!mft.haveData || mft.dataResident || mft.runs.empty())
    {
        err = L"Cannot locate the $MFT data runs.";
        return false;
    }

    ULONGLONG totalRecords = m_mftValidLen / m_recSize;
    ULONGLONG recNo = 0;
    DWORD chunkClusters = kChunkBytes / m_bpc;
    std::vector<BYTE> chunk(kChunkBytes);
    std::vector<BYTE> pending;
    pending.reserve(kChunkBytes + m_recSize);

    for (const DataRun& run : mft.runs)
    {
        if (recNo >= totalRecords)
            break;
        if (run.lcn < 0)
        {
            pending.clear();
            recNo += run.clusters * m_bpc / m_recSize;
            continue;
        }
        for (ULONGLONG c = 0; c < run.clusters && recNo < totalRecords; )
        {
            DWORD n = (DWORD)std::min<ULONGLONG>(chunkClusters, run.clusters - c);
            DWORD bytes = n * m_bpc;
            if (!ReadBytes((ULONGLONG)(run.lcn + (LONGLONG)c) * m_bpc, chunk.data(), bytes))
                memset(chunk.data(), 0, bytes);     // unreadable sectors: records there are skipped

            pending.insert(pending.end(), chunk.begin(), chunk.begin() + bytes);
            size_t off = 0;
            while (pending.size() - off >= m_recSize && recNo < totalRecords)
            {
                ProcessRecord(pending.data() + off, recNo, out);
                off += m_recSize;
                ++recNo;
            }
            pending.erase(pending.begin(), pending.begin() + off);
            c += n;

            if (progress && !progress(recNo, totalRecords))
            {
                err = L"Scan cancelled.";
                return false;
            }
        }
    }

    MergeExtensionNames(out);

    std::unordered_map<ULONGLONG, CStringW> cache;
    for (DeletedEntry& e : out)
    {
        if (e.parentRecord == kRootRecord)
            e.path = L"";
        else if (ParentMatches(e.parentRecord, e.parentSeq))
            e.path = DirPath(e.parentRecord, cache, 0);
        else
            e.path = L"\\$Orphaned";
    }

    // Every folder resolved above lies on the path of a deleted item; deleted folders are added too.
    for (const DeletedEntry& e : out)
        if (e.isDir)
            DirPath(e.recordNo, cache, 0);
    for (const auto& kv : cache)
    {
        auto it = m_dirs.find(kv.first);
        if (it != m_dirs.end())
            folders.push_back({ kv.second, it->second.inUse });
    }
    return true;
}

bool NtfsVolume::ExtractTo(const DeletedEntry& e, HANDLE hOut, const ProgressFn& progress, CString& warn, CString& err)
{
    DWORD written = 0;
    if (e.resident)
    {
        DWORD len = (DWORD)std::min<ULONGLONG>(e.size, e.residentData.size());
        if (len && !WriteFile(hOut, e.residentData.data(), len, &written, nullptr))
        {
            err.Format(L"Write failed (error %u)", GetLastError());
            return false;
        }
        return true;
    }

    std::vector<BYTE> buf(kChunkBytes);
    DWORD chunkClusters = kChunkBytes / m_bpc;
    ULONGLONG pos = 0;
    int badReads = 0;

    for (const DataRun& r : e.runs)
    {
        for (ULONGLONG c = 0; c < r.clusters && pos < e.size; )
        {
            ULONGLONG remaining = e.size - pos;
            DWORD n = (DWORD)std::min<ULONGLONG>(chunkClusters, r.clusters - c);
            n = (DWORD)std::min<ULONGLONG>(n, (remaining + m_bpc - 1) / m_bpc);
            DWORD bytes = n * m_bpc;
            DWORD toWrite = (DWORD)std::min<ULONGLONG>(bytes, remaining);

            if (r.lcn < 0 || pos >= e.initSize)
                memset(buf.data(), 0, bytes);
            else if (!ReadBytes((ULONGLONG)(r.lcn + (LONGLONG)c) * m_bpc, buf.data(), bytes))
            {
                memset(buf.data(), 0, bytes);
                ++badReads;
            }
            if (pos < e.initSize && pos + toWrite > e.initSize)
                memset(buf.data() + (e.initSize - pos), 0, (size_t)(pos + toWrite - e.initSize));

            if (!WriteFile(hOut, buf.data(), toWrite, &written, nullptr) || written != toWrite)
            {
                err.Format(L"Write failed (error %u)", GetLastError());
                return false;
            }
            pos += toWrite;
            c += n;
            if (progress && !progress(pos, e.size))
            {
                err = L"Cancelled";
                return false;
            }
        }
        if (pos >= e.size)
            break;
    }

    if (pos < e.size)
        warn.Format(L"cluster map ended early: %llu of %llu bytes recovered", pos, e.size);
    if (badReads)
    {
        CString w;
        w.Format(L"%s%d unreadable chunk(s) zero-filled", warn.IsEmpty() ? L"" : L"; ", badReads);
        warn += w;
    }
    return true;
}
