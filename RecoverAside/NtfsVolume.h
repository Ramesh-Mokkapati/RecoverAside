// NtfsVolume.h : strictly read-only NTFS MFT scanner and file extractor.
// The source volume is opened with GENERIC_READ only; nothing is ever written to it.
#pragma once

struct DataRun
{
    LONGLONG  lcn;          // -1 = sparse run
    ULONGLONG clusters;
};

enum class RecoverState { Good, Partial, Overwritten, Unsupported };

struct DeletedEntry
{
    ULONGLONG recordNo = 0;
    WORD      seq = 0;
    ULONGLONG parentRecord = 0;
    WORD      parentSeq = 0;
    CStringW  name;
    CStringW  path;             // parent folder relative to the volume root, e.g. L"\\Users\\Bob"
    bool      isDir = false;
    ULONGLONG size = 0;         // file size; for folders, total size of deleted contents
    ULONGLONG initSize = 0;     // bytes beyond this are zero on disk
    FILETIME  created = {}, modified = {}, accessed = {};
    bool      resident = false;
    std::vector<BYTE>    residentData;
    std::vector<DataRun> runs;
    RecoverState state = RecoverState::Good;
    int       overwrittenPct = 0;
    CStringW  note;
};

// A folder on the path of at least one deleted item (live or deleted itself).
struct FolderInfo
{
    CStringW path;              // relative to the volume root, e.g. L"\\Users\\Bob"
    bool     live;              // still exists on the volume
};

// Return false to cancel.
using ProgressFn = std::function<bool(ULONGLONG done, ULONGLONG total)>;

class NtfsVolume
{
public:
    NtfsVolume() = default;
    ~NtfsVolume() { Close(); }
    NtfsVolume(const NtfsVolume&) = delete;
    NtfsVolume& operator=(const NtfsVolume&) = delete;

    bool Open(const CString& device, CString& err);
    void Close();

    bool Scan(std::vector<DeletedEntry>& out, std::vector<FolderInfo>& folders, const ProgressFn& progress, CString& err);
    bool ExtractTo(const DeletedEntry& e, HANDLE hOut, const ProgressFn& progress, CString& warn, CString& err);

    DWORD BytesPerCluster() const { return m_bpc; }

private:
    struct DirNode
    {
        ULONGLONG parentRef;
        CStringW  name;
        WORD      seq;
        bool      inUse;
        int       nameRank;     // -1 = no name in the base record (yet)
    };
    // Best $FILE_NAME found in an extension record, keyed by base record.
    struct ExtName
    {
        WORD      baseSeq = 0;
        int       rank = -1;
        CStringW  name;
        ULONGLONG parentRef = 0;
    };
    struct ParsedRecord;

    bool ReadBytes(ULONGLONG offset, void* buf, DWORD len);
    bool LoadBitmap(CString& err);
    bool IsAllocated(ULONGLONG lcn) const;
    bool ParseRecord(BYTE* rec, ParsedRecord& pr, bool wantData) const;
    void ProcessRecord(BYTE* rec, ULONGLONG recNo, std::vector<DeletedEntry>& out);
    void Evaluate(DeletedEntry& e, const ParsedRecord& pr) const;
    bool ParentMatches(ULONGLONG parentRec, WORD parentSeq) const;
    CStringW DirPath(ULONGLONG rec, std::unordered_map<ULONGLONG, CStringW>& cache, int depth) const;

    HANDLE    m_h = INVALID_HANDLE_VALUE;
    DWORD     m_bpc = 0;            // bytes per cluster
    DWORD     m_bps = 0;            // bytes per sector
    DWORD     m_recSize = 0;        // bytes per MFT record
    ULONGLONG m_totalClusters = 0;
    LONGLONG  m_mftStartLcn = 0;
    ULONGLONG m_mftValidLen = 0;
    std::vector<BYTE> m_bitmap;
    std::unordered_map<ULONGLONG, DirNode> m_dirs;
    std::unordered_map<ULONGLONG, ExtName> m_extNames;
    std::vector<int> m_entryRanks;      // name rank of each scanned DeletedEntry
    void MergeExtensionNames(std::vector<DeletedEntry>& out);
};
