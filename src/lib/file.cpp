#include <lib/file.h>
#include <platform/config.h>
#include <platform/filesystem.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <strings.h>
#include <vector>

// Host: with a separate CD folder (the read-only game folder), each drive layers the data folder over it; reads fall
// back to it, writes stay in the data folder.
namespace win32
{

// Guest current directory: "X:\" plus components, always ending in a backslash.
static std::string s_currentDirectory = "C:\\";
static std::string s_dataDirectory = "ux0:data/NeedForSpeedUnderground2/";
static std::string s_cdDirectory = "ux0:data/NeedForSpeedUnderground2/";

// A file the guest sees as existing and empty, with no host file.
static const int EmptyFile = -2;

// What an open does with its path in the host's layered folders.
enum class Access
{
    Read,       // the folder that holds the path, else the data folder
    Create,     // the data folder, its parent folders made
    Modify      // the data folder, after copying the CD folder's file
};

static bool isSeparator(char c)
{
    return c == '\\' || c == '/';
}

// Resolves "." and ".." in a guest path that starts with "X:"; the result ends with a backslash when the path did.
static std::string resolveDots(const std::string& path)
{
    std::string result = path.substr(0, 2);
    result[0] = char(toupper(result[0]));
    std::vector<std::string> parts;
    std::string::size_type start = 2;
    while (start <= path.size())
    {
        std::string::size_type end = start;
        while (end < path.size() && !isSeparator(path[end]))
            end++;
        std::string part = path.substr(start, end - start);
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        start = end + 1;
    }
    for (std::vector<std::string>::const_iterator it = parts.begin(); it != parts.end(); ++it)
        result += "\\" + *it;
    if (parts.empty() || (!path.empty() && isSeparator(path.back())))
        result += "\\";
    return result;
}

// resolveDots with a trailing backslash.
static std::string normalizeGuestDirectory(const std::string& path)
{
    std::string result = resolveDots(path);
    if (!isSeparator(result.back()))
        result += "\\";
    return result;
}

// Makes a guest path absolute ("X:\...") using the current directory.
static std::string absoluteGuestPath(const std::string& path)
{
    if (path.size() >= 2 && path[1] == ':')
        return path;
    if (!path.empty() && isSeparator(path[0]))
        return s_currentDirectory.substr(0, 2) + path;
    return s_currentDirectory + path;
}

static std::string normalizePath(const std::string& input, bool keepCase = false);

// Whether a normalised path is the save folder (C:\save, see shfolder) or below it.
static bool isSaveRelative(const std::string& relative)
{
    return strncasecmp(relative.c_str(), "save", 4) == 0 && (relative.size() == 4 || relative[4] == '/');
}

// Normalised path below the drive root, and the drive letter. The save folder keeps the game's letter case.
static std::string guestRelative(const std::string& path, char* drive)
{
    std::string full = resolveDots(absoluteGuestPath(path));
    std::string rest = full.substr(2);
    while (!rest.empty() && isSeparator(rest[0]))
        rest.erase(0, 1);
    *drive = char(toupper(full[0]));
    const std::string lowered = normalizePath(rest);
    return isSaveRelative(lowered) ? normalizePath(rest, true) : lowered;
}

// Save folder override (save_dir); empty for the folder below the data folder.
static const std::string& saveDirectory()
{
    static const std::string s_directory = []
    {
        std::string directory = pf::Config::getString("save_dir", "");
        if (!directory.empty() && directory.back() != '/')
            directory += '/';
        return directory;
    }();
    return s_directory;
}

static std::atomic<unsigned> s_saveCommits{0};

unsigned File::saveCommits()
{
    return s_saveCommits.load();
}

static bool saveLogging()
{
    static const bool s_log = pf::Config::getBool("save_log", false);
    return s_log;
}

void File::logSave(const char* guestPath, const char* format, ...)
{
    if (!saveLogging())
        return;
    if (guestPath)
    {
        char drive = 0;
        if (!isSaveRelative(guestRelative(guestPath, &drive)))
            return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    pf::logInfo("save: %s", text);
}

static bool hostExists(const std::string& path)
{
    struct stat status;
    return stat(path.c_str(), &status) == 0;
}

// A fixed name below the save folder for the temporary file of a save file (the game never lists that folder).
static std::string temporaryName(const std::string& target)
{
    uint32_t hash = 2166136261u;
    for (char c : target)
        hash = (hash ^ uint8_t(c)) * 16777619u;
    char name[32];
    snprintf(name, sizeof(name), "/.tmp/%08x.part", unsigned(hash));
    std::string folder = saveDirectory().empty() ? s_dataDirectory + "save" : saveDirectory();
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    mkdir(folder.c_str(), 0777);
    mkdir((folder + "/.tmp").c_str(), 0777);
    return folder + name;
}

// rename over the target; the Vita refuses an existing target, so that one goes first.
static bool replaceFile(const std::string& from, const std::string& to)
{
    if (rename(from.c_str(), to.c_str()) == 0)
        return true;
    struct stat status;
    return stat(to.c_str(), &status) == 0 && unlink(to.c_str()) == 0 && rename(from.c_str(), to.c_str()) == 0;
}

// SPEED2.EXE skips its disc check when "foobar" exists (0x5C0D6F).
static bool isDiscSwitch(const char* path)
{
    char drive = 0;
    return guestRelative(path, &drive) == "foobar" && (drive == 'C' || drive == 'D');
}

#ifndef __vita__
static bool layered()
{
    return s_cdDirectory != s_dataDirectory;
}

static std::string layerPath(const std::string& root, const std::string& relative)
{
    return root + pf::resolveCase(root, relative);
}

static void makeParents(const std::string& path)
{
    for (size_t slash = path.find('/', s_dataDirectory.size()); slash != std::string::npos;
         slash = path.find('/', slash + 1))
        mkdir(path.substr(0, slash).c_str(), 0777);
}

static bool copyFile(const std::string& from, const std::string& to)
{
    const int in = open(from.c_str(), O_RDONLY);
    const int out = in < 0 ? -1 : open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    bool ok = in >= 0 && out >= 0;
    char buffer[65536];
    for (ssize_t n; ok && (n = ::read(in, buffer, sizeof(buffer))) != 0;)
        ok = n > 0 && ::write(out, buffer, size_t(n)) == n;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return ok;
}

static std::string layeredPath(const std::string& relative, Access access)
{
    const std::string upper = layerPath(s_dataDirectory, relative);
    if (hostExists(upper))
        return upper;
    const std::string lower = layerPath(s_cdDirectory, relative);
    if (access == Access::Read)
        return hostExists(lower) ? lower : upper;
    makeParents(upper);
    struct stat status;
    if (access == Access::Modify && stat(lower.c_str(), &status) == 0 && S_ISREG(status.st_mode)
        && !copyFile(lower, upper))
        pf::logError("file %s: copy to %s failed (errno %d)", lower.c_str(), upper.c_str(), errno);
    return upper;
}
#endif

// Guest path -> host path in the data or CD folder.
static std::string moveToRoot(const std::string& path, Access access = Access::Read)
{
    std::string device = path;
    for (size_t i = 0; i < device.size(); ++i)
        device[i] = char(tolower(device[i]));
#ifdef __vita__
    (void)access;
    // No null device on the Vita: use a scratch file. Console output goes to the debug tty.
    if (device == "nul" || device == "conin$")
        return s_dataDirectory + "nul.tmp";
    if (device == "conout$")
        return "tty0:";
#else
    if (device == "nul" || device == "conin$")
        return "/dev/null";
    if (device == "conout$")
        return "/dev/stderr";
#endif
    char drive = 0;
    const std::string relative = guestRelative(path, &drive);
    if (!saveDirectory().empty() && isSaveRelative(relative))
    {
        const std::string below = relative.size() > 5 ? relative.substr(5) : std::string();
        return saveDirectory() + pf::resolveCase(saveDirectory(), below);
    }
#ifndef __vita__
    if (layered())
        return layeredPath(relative, access);
#endif
    const std::string& root = drive == 'D' ? s_cdDirectory : s_dataDirectory;
    return root + pf::resolveCase(root, relative);
}

// Lower-cases, converts separators and collapses repeats; the Vita filesystem is case-insensitive.
static std::string normalizePath(const std::string& input, bool keepCase)
{
    std::string path;
    path.reserve(input.size());
    for (std::string::const_iterator it = input.begin(); it != input.end(); ++it)
    {
        char c = keepCase ? *it : char(tolower(*it));
        if (c == '\\')
            c = '/';
        if (c == '/' && !path.empty() && path.back() == '/')
            continue;
        path += c;
    }
    return path;
}

// Case-insensitive match supporting '*' and '?'.
static bool wildcardMatch(const char* pattern, const char* name)
{
    const char* star = nullptr;
    const char* starName = nullptr;
    while (*name)
    {
        if (*pattern == '*')
        {
            star = pattern++;
            starName = name;
        }
        else if (*pattern == '?' || tolower(*pattern) == tolower(*name))
        {
            pattern++;
            name++;
        }
        else if (star)
        {
            pattern = star + 1;
            name = ++starName;
        }
        else
        {
            return false;
        }
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == 0;
}

FileEnumerator::FileEnumerator(const char* filename)
    :   m_pattern(moveToRoot(filename))
    ,   m_index(-1)
{
    pf::logDebug("Enumerate %s", m_pattern.c_str());
    std::vector<std::string> patterns(1, m_pattern);
#ifndef __vita__
    if (layered())
    {
        char drive = 0;
        const std::string relative = guestRelative(filename, &drive);
        patterns = { layerPath(s_dataDirectory, relative), layerPath(s_cdDirectory, relative) };
    }
#endif
    for (std::string& pattern : patterns)
    {
        /* Win32 "*.*" means every file */
        size_t pos = pattern.find("*.*");
        if (pos != std::string::npos)
            pattern.replace(pos, 3, "*");
        std::string dirPath, filePattern;
        size_t lastSlash = pattern.rfind('/');
        if (lastSlash != std::string::npos)
        {
            dirPath = pattern.substr(0, lastSlash);
            filePattern = pattern.substr(lastSlash + 1);
        }
        else
        {
            dirPath = s_dataDirectory;
            filePattern = pattern;
        }
        const size_t earlier = m_files.size();
        DIR* d = opendir(dirPath.c_str());
        if (d)
        {
            // Windows lists "." and ".." below the drive root; the Vita's readdir may not, so they are made here.
            const bool root = dirPath + "/" == s_dataDirectory || dirPath + "/" == s_cdDirectory;
            for (const char* dots : { ".", ".." })
            {
                bool listed = false;
                for (size_t i = 0; i < m_files.size() && !listed; ++i)
                    listed = strcmp(m_files[i].cFileName, dots) == 0;
                if (!root && !listed && wildcardMatch(filePattern.c_str(), dots))
                {
                    WIN32_FIND_DATAA data;
                    memset(&data, 0, sizeof(data));
                    data.dwFileAttributes = 16;
                    strcpy(data.cFileName, dots);
                    m_files.push_back(data);
                }
            }
            struct dirent* entry;
            while ((entry = readdir(d)) != nullptr)
            {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;
                bool listed = false;
                for (size_t i = 0; i < earlier && !listed; ++i)
                    listed = strcasecmp(m_files[i].cFileName, entry->d_name) == 0;
                if (!listed && wildcardMatch(filePattern.c_str(), entry->d_name))
                {
                    std::string fullPath = dirPath + "/" + entry->d_name;
                    struct stat buffer;
                    memset(&buffer, 0, sizeof(buffer));
                    stat(fullPath.c_str(), &buffer);
                    WIN32_FIND_DATAA data;
                    memset(&data, 0, sizeof(data));
                    data.dwFileAttributes = S_ISDIR(buffer.st_mode) ? 16 : 0;
                    data.nFileSizeHigh = 0;
                    data.nFileSizeLow = x86::reg32(buffer.st_size);
                    strncpy(data.cFileName, entry->d_name, sizeof(data.cFileName)-1);
                    m_files.push_back(data);
                }
            }
            closedir(d);
        }
    }
    m_pattern = patterns[0];
}

FileEnumerator::~FileEnumerator()
{
}

bool FileEnumerator::findNext(WIN32_FIND_DATAA *data)
{
    m_index++;
    if (size_t(m_index) >= m_files.size())
    {
        return false;
    }
    else
    {
        memcpy(data, &m_files[m_index], sizeof(*data));
        return true;
    }
}

struct ReadFilterEntry
{
    std::string             suffix;
    File::ReadFilter        filter;
};

// Written before guest code runs, read-only afterwards.
static std::vector<ReadFilterEntry> s_readFilters;

void File::setReadFilter(const char* pathSuffix, ReadFilter filter)
{
    s_readFilters.push_back(ReadFilterEntry{ pathSuffix, filter });
}

void File::initReadFilter()
{
    m_readFilter = nullptr;
    for (const ReadFilterEntry& entry : s_readFilters)
    {
        if (m_filename.size() >= entry.suffix.size()
            && m_filename.compare(m_filename.size() - entry.suffix.size(), entry.suffix.size(), entry.suffix) == 0)
            m_readFilter = entry.filter;
    }
}

// Read-only data files closed recently, kept open: the game opens its stream file per section, and an open takes
// milliseconds on the Vita (setting file_handle_cache).
struct ParkedFile
{
    std::string name;
    int         file;
    bool        sizeKnown;
    x86::reg32  size;
};
// A friend's car in free roam opens a dozen files and more (its GEOMETRY.BIN and TEXTURES.BIN, the shared part and
// rim packs) and each again for its size (bFileSize, 0x57CAC0, opens and closes it), while the streamer keeps its own
// open: with 4 kept, most of those opens missed and each took 10 to 20 ms on the game thread (the Vita, 2026-11-23,
// CarLoader::Load's 0x57CA10, three of them a car).
static const size_t ParkedMax = 48;
static std::vector<ParkedFile> s_parked;    // oldest first; parkedLock()
// Read-only data files that were not there, by host path: the game probes such files again and again, and each
// failed open costs as much as one that works.  Forgotten with the parked files (any write, delete or move).
static std::vector<std::string> s_missing;  // parkedLock()
static const size_t MissingMax = 256;

static pf::LwMutex& parkedLock()
{
    static pf::LwMutex s_lock{"file_parked"};
    return s_lock;
}

static bool parking()
{
    static const bool s_on = pf::Config::getBool("file_handle_cache", true);
    return s_on;
}

void File::dropParked()
{
    pf::LwLockGuard guard(parkedLock());
    for (const ParkedFile& parked : s_parked)
        close(parked.file);
    s_parked.clear();
    s_missing.clear();
}

// File::prewarm's worker: guest paths queued, opened one after another at low priority.
namespace
{
std::vector<std::string> s_warmQueue;   // parkedLock()

bool isParkedOrMissing(const std::string& host)
{
    for (const ParkedFile& parked : s_parked)
        if (parked.name == host)
            return true;
    for (const std::string& missing : s_missing)
        if (missing == host)
            return true;
    return false;
}

#ifdef __vita__
// Made by File::prewarm before the worker starts: a function-local static here was first constructed on the game
// thread while the worker's first wait reached its guard, and the guard threw recursive_init_error (the Vita,
// 2026-11-26: std::terminate on file_prewarm as free roam's players came in).
pf::Semaphore* s_warmSignal = nullptr;

int warmWorker(void*)
{
    for (;;)
    {
        s_warmSignal->wait();
        std::string host;
        {
            pf::LwLockGuard guard(parkedLock());
            if (s_warmQueue.empty())
                continue;
            host = s_warmQueue.front();
            s_warmQueue.erase(s_warmQueue.begin());
            if (isParkedOrMissing(host))
                continue;
        }
        const int fd = open(host.c_str(), O_RDONLY, 0);
        const int code = fd < 0 ? errno : 0;
        int oldest = -1;
        {
            pf::LwLockGuard guard(parkedLock());
            if (fd < 0)
            {
                if (code == ENOENT)
                {
                    if (s_missing.size() >= MissingMax)
                        s_missing.erase(s_missing.begin());
                    s_missing.push_back(host);
                }
            }
            else if (isParkedOrMissing(host))
                oldest = fd;   // (the game opened and closed it meanwhile)
            else
            {
                if (s_parked.size() >= ParkedMax)
                {
                    oldest = s_parked.front().file;
                    s_parked.erase(s_parked.begin());
                }
                s_parked.push_back(ParkedFile{ host, fd, false, 0 });
            }
        }
        if (oldest >= 0)
            close(oldest);
    }
    return 0;
}
#endif
}

void File::prewarm(const char* guestPath)
{
#ifdef __vita__
    if (!guestPath || !parking())
        return;
    const std::string host = moveToRoot(guestPath);
    {
        pf::LwLockGuard guard(parkedLock());
        if (isParkedOrMissing(host) || s_warmQueue.size() >= 64)
            return;
        for (const std::string& queued : s_warmQueue)
            if (queued == host)
                return;
        s_warmQueue.push_back(host);
    }
    static pf::Thread* s_thread = nullptr;   // (the game thread only)
    if (!s_thread)
    {
        s_warmSignal = new pf::Semaphore("file_prewarm", 0);
        s_thread = new pf::Thread("file_prewarm", warmWorker, nullptr, pf::Thread::PriorityLow, 32 * 1024);
        s_thread->start();
    }
    s_warmSignal->post();
#else
    (void)guestPath;
#endif
}

File::File(int file, const char* name)
    :   m_file(dup(file))
    ,   m_filename(name)
{
    initReadFilter();
}

File::File(const File& other)
    :   m_file(other.m_file == EmptyFile ? EmptyFile : open(other.m_filename.c_str(), O_RDONLY, 0664))
    ,   m_filename(other.m_filename)
    ,   m_readFilter(other.m_readFilter)
    ,   m_readOnly(true)
{
    pf::logDebug("opening file %s for Map", m_filename.c_str());
}

static bool writesData(x86::reg32 mode)
{
    return (mode & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
}

static Access accessOf(x86::reg32 mode, x86::reg32 creationFlags)
{
    if (writesData(mode))
        return creationFlags == CREATE_ALWAYS ? Access::Create : Access::Modify;
    return creationFlags == OPEN_EXISTING || creationFlags == OPEN_ALWAYS ? Access::Read : Access::Create;
}

File::File(const char* path, x86::reg32 mode, x86::reg32 flags, x86::reg32 creationFlags)
    :   m_file(-1)
    ,   m_filename(moveToRoot(path, accessOf(mode, creationFlags)))
{
    RC_USE(flags);
    initReadFilter();
    {
        char drive = 0;
        m_save = isSaveRelative(guestRelative(path, &drive));
    }
    // Access 0 only queries the file: opened for reading.
    const bool reads = (mode & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    const bool writes = writesData(mode);
    m_parkable = !writes && creationFlags == OPEN_EXISTING && !m_save && parking();
    if (writes)
        dropParked();
    if (m_parkable)
    {
        pf::LwLockGuard guard(parkedLock());
        for (size_t i = s_parked.size(); i-- > 0; )
        {
            if (s_parked[i].name != m_filename)
                continue;
            m_file = s_parked[i].file;
            m_sizeKnown = s_parked[i].sizeKnown;
            m_size = s_parked[i].size;
            s_parked.erase(s_parked.begin() + ptrdiff_t(i));
            m_existed = true;
            m_readOnly = true;
            m_seekPending = true;
            return;
        }
        for (const std::string& missing : s_missing)
        {
            if (missing != m_filename)
                continue;
            m_error = ENOENT;
            m_existed = false;
            return;
        }
    }
    else
    {
        m_existed = hostExists(m_filename);
    }
    int openFlags = writes ? (reads ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch(creationFlags)
    {
    case CREATE_NEW:
        openFlags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        openFlags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_ALWAYS:
        openFlags |= O_CREAT;
        break;
    case OPEN_EXISTING:
        break;
    case TRUNCATE_EXISTING:
        openFlags |= O_TRUNC;
        break;
    default:
        pf::logError("CreateFile %s: unknown creation disposition %u", m_filename.c_str(), unsigned(creationFlags));
        return;
    }
    // The scratch file standing in for the null device may not exist yet.
    if (m_filename == s_dataDirectory + "nul.tmp")
        openFlags |= O_CREAT;
    if (!writes && (creationFlags == OPEN_EXISTING || creationFlags == OPEN_ALWAYS) && isDiscSwitch(path)
        && !hostExists(m_filename))
    {
        m_file = EmptyFile;
        m_readOnly = true;
        pf::logInfo("file %s: the disc check switch, opened as an empty file", path);
        return;
    }

    // A save file is written to a temporary file that replaces the old one on close, so a power cut or a closed
    // application leaves the old save whole.
    static const bool s_atomic = pf::Config::getBool("save_atomic", true);
    const size_t slash = m_filename.rfind('/');
    if (s_atomic && m_save && writes && hostExists(m_filename.substr(0, slash == std::string::npos ? 0 : slash))
        && (creationFlags == CREATE_ALWAYS || (creationFlags == TRUNCATE_EXISTING && m_existed)))
    {
        const std::string temporary = temporaryName(m_filename);
        m_file = open(temporary.c_str(), openFlags, 0664);
        if (m_file >= 0)
        {
            m_target = m_filename;
            m_filename = temporary;
        }
    }
    if (m_file < 0)
        m_file = open(m_filename.c_str(), openFlags, 0664);
    m_error = m_file < 0 ? errno : 0;
    m_readOnly = !writes;
    if (m_parkable)
    {
        m_existed = m_file >= 0;
        if (m_file < 0 && m_error == ENOENT)
        {
            pf::LwLockGuard guard(parkedLock());
            if (s_missing.size() >= MissingMax)
                s_missing.erase(s_missing.begin());
            s_missing.push_back(m_filename);
        }
    }
    // The game probes files that do not exist: not an error.
    if (m_file == -1)
    {
        pf::logInfo("file %s not opened (errno %d)", m_filename.c_str(), m_error);
        if (m_save)
            logSave(nullptr, "open %s failed (errno %d)", path, m_error);
    }
    else
    {
        pf::logDebug("opening file %s", m_filename.c_str());
        if (m_save)
            logSave(nullptr, "open %s -> %s (%s%s, disposition %u)", path, m_filename.c_str(),
                    writes ? "write" : "read", m_target.empty() ? "" : ", temporary file", unsigned(creationFlags));
    }
}

File::~File()
{
    if (m_file >= 0 && m_parkable)
    {
        int oldest = -1;
        {
            pf::LwLockGuard guard(parkedLock());
            if (s_parked.size() >= ParkedMax)
            {
                oldest = s_parked.front().file;
                s_parked.erase(s_parked.begin());
            }
            s_parked.push_back(ParkedFile{ m_filename, m_file, m_sizeKnown, m_size });
        }
        if (oldest >= 0)
            close(oldest);
        m_file = -1;
        return;
    }
    if (m_file >= 0)
    {
        if (m_dirty && m_save && !flush())
            pf::logError("file %s: not saved", (m_target.empty() ? m_filename : m_target).c_str());
        close(m_file);
        m_file = -1;
        if (!m_target.empty() && !replaceFile(m_filename, m_target))
            pf::logError("file %s: could not replace %s (errno %d)", m_filename.c_str(), m_target.c_str(), errno);
        else if (m_save)
        {
            if (!m_target.empty())
                ++s_saveCommits;
            logSave(nullptr, "close %s", (m_target.empty() ? m_filename : m_target).c_str());
        }
        pf::logDebug("closing file %s", m_filename.c_str());
    }
}

// fsync of a save file; a temporary file replaces the old one when the file is closed.
bool File::flush()
{
    if (m_file < 0 || !m_dirty)
        return true;
    static const bool s_sync = pf::Config::getBool("save_sync", true);
    if (!s_sync || fsync(m_file) == 0)
    {
        m_dirty = false;
        logSave(nullptr, "sync %s", m_filename.c_str());
        return true;
    }
    m_error = errno;
    pf::logError("file %s: fsync failed (errno %d)", m_filename.c_str(), m_error);
    return false;
}

x86::reg32 File::seek(x86::reg32 mode, x86::sreg32 offset)
{
    if (m_file == EmptyFile)
    {
        m_position = (mode == 1 ? m_position : 0) + offset;
        return x86::reg32(m_position);
    }
    // Vita newlib's lseek dereferences a null descriptor for an invalid fd (unlike read/write/fstat),
    // e.g. the stdin handle: fd 0 cannot be dup'ed. Fail like Windows does for a console handle.
    if (m_file < 0)
        return x86::reg32(-1);
    static const bool s_lazy = pf::Config::getBool("file_lazy_seek", true);
    if (s_lazy && m_readOnly && mode < 2)
    {
        const off_t target = (mode == 1 ? m_position : 0) + offset;
        if (target < 0)
            return x86::reg32(-1);
        m_seekPending = m_seekPending || target != m_position;
        m_position = target;
        return x86::reg32(target);
    }
    if (m_seekPending)
    {
        lseek(m_file, m_position, SEEK_SET);
        m_seekPending = false;
    }
    int seekMode = 0;
    switch(mode)
    {
    case 0:
        seekMode = SEEK_SET;
        break;
    case 1:
        seekMode = SEEK_CUR;
        break;
    case 2:
        seekMode = SEEK_END;
        break;
    default:
        RC_ASSERT(false);
        break;
    }
    const off_t position = lseek(m_file, offset, seekMode);
    if (position >= 0)
        m_position = position;
    return x86::reg32(position);
}

x86::reg32 File::read(void* buffer, x86::reg32 max, x86::reg32* totalRead)
{
    if (m_file < 0)
    {
        *totalRead = 0;
        return m_file == EmptyFile ? 1 : 0;
    }
    if (m_seekPending)
    {
        if (lseek(m_file, m_position, SEEK_SET) != m_position)
        {
            m_error = errno;
            pf::logError("file %s: seek to %ld failed (errno %d)", m_filename.c_str(), long(m_position), m_error);
            *totalRead = 0;
            return 0;
        }
        m_seekPending = false;
    }
    const off_t offset = m_readFilter ? lseek(m_file, 0, SEEK_CUR) : 0;
    // The system may return less than asked for before the end of the file: read until the buffer is full.
    // A read the system cut short (the game suspended with the PS button while the read waited, the memory card
    // busy) fails at once; the file is then opened again at the same position and the read done once more.
    char* bytes = static_cast<char*>(buffer);
    x86::reg32 got = 0;
    int retries = 2;
    bool failed = false;
    while (got < max)
    {
        const ssize_t count = ::read(m_file, bytes + got, max - got);
        if (count > 0)
        {
            got += x86::reg32(count);
            continue;
        }
        if (count == 0)
            break;
        const int code = errno;
        if (code == EINTR)
            continue;
        m_error = code;
        const bool reopened = m_readOnly && retries > 0 && reopenAt(m_position + off_t(got));
        pf::logError("file %s: read of %u bytes at %ld failed (errno %d)%s", m_filename.c_str(), unsigned(max - got),
                     long(m_position + off_t(got)), code, reopened ? ", opened again" : "");
        if (!reopened)
        {
            failed = true;
            break;
        }
        --retries;
    }
    *totalRead = got;
    m_position += off_t(got);
    if (m_readFilter && offset >= 0 && got)
        m_readFilter(x86::reg32(offset), static_cast<x86::reg8*>(buffer), got);
    return !failed;
}

bool File::reopenAt(off_t position)
{
    const int file = open(m_filename.c_str(), O_RDONLY, 0664);
    if (file < 0)
        return false;
    if (lseek(file, position, SEEK_SET) != position)
    {
        close(file);
        return false;
    }
    if (m_file >= 0)
        close(m_file);
    m_file = file;
    return true;
}

x86::reg32 File::write(const void* buffer, x86::reg32 max, x86::reg32* totalWritten)
{
    const char* bytes = static_cast<const char*>(buffer);
    x86::reg32 done = 0;
    while (done < max && m_file >= 0)
    {
        const ssize_t count = ::write(m_file, bytes + done, max - done);
        if (count > 0)
        {
            done += x86::reg32(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        m_error = count < 0 ? errno : ENOSPC;
        pf::logError("file %s: write of %u bytes failed after %u (errno %d)", m_filename.c_str(), unsigned(max),
                     unsigned(done), m_error);
        break;
    }
    *totalWritten = done;
    if (done)
        m_dirty = true;
    return done == max;
}

x86::reg32 File::truncate()
{
    pf::logDebug("Truncating file %s", m_filename.c_str());
    if (m_file < 0)
        return 0;
    m_dirty = true;
    logSave(nullptr, "truncate %s at %ld", m_filename.c_str(), long(lseek(m_file, 0, SEEK_CUR)));
    return ftruncate(m_file, lseek(m_file, 0, SEEK_CUR)) == 0;
}

x86::reg32 File::size() const
{
    if (m_sizeKnown)
        return m_size;
    struct stat buffer;
    memset(&buffer, 0, sizeof(buffer));
    fstat(m_file, &buffer);
    if (m_parkable && m_file >= 0)
    {
        m_sizeKnown = true;
        m_size = x86::reg32(buffer.st_size);
    }
    return x86::reg32(buffer.st_size);
}

// Win32 form: no trailing backslash except for a drive root.
std::string File::getCurrentDirectory()
{
    std::string result = s_currentDirectory;
    if (result.size() > 3)
        result.pop_back();
    return result;
}

bool File::setCurrentDirectory(const char* path)
{
    std::string directory = normalizeGuestDirectory(absoluteGuestPath(path));
    std::string host = moveToRoot(directory);
    struct stat buffer;
    memset(&buffer, 0, sizeof(buffer));
    if (stat(host.c_str(), &buffer) != 0 || !S_ISDIR(buffer.st_mode))
    {
        pf::logError("SetCurrentDirectory %s: not found (%s)", path, host.c_str());
        return false;
    }
    s_currentDirectory = directory;
    pf::logDebug("current directory %s", s_currentDirectory.c_str());
    return true;
}

void File::setDataDirectory(const char* path)
{
    s_dataDirectory = path;
    if (s_dataDirectory.empty() || s_dataDirectory.back() != '/')
        s_dataDirectory.append("/");
}

void File::setCdDirectory(const char* path)
{
    s_cdDirectory = path;
    if (s_cdDirectory.empty() || s_cdDirectory.back() != '/')
        s_cdDirectory.append("/");
}

bool File::diskSpace(x86::reg64* total, x86::reg64* free)
{
    const bool known = pf::diskSpace(saveDirectory().empty() ? s_dataDirectory : saveDirectory(), total, free);
    logSave(nullptr, "free space %s: %llu of %llu bytes", known ? "read" : "unknown", (unsigned long long)*free,
            (unsigned long long)*total);
    return known;
}

std::string File::hostPath(const char* path)
{
    std::string host = moveToRoot(path);
    while (host.size() > 1 && host.back() == '/')
        host.pop_back();
    return host;
}

bool File::writable(const std::string& hostPath)
{
#ifndef __vita__
    if (layered() && hostPath.compare(0, s_dataDirectory.size(), s_dataDirectory) != 0
        && (saveDirectory().empty() || hostPath.compare(0, saveDirectory().size(), saveDirectory()) != 0))
        return false;
#else
    (void)hostPath;
#endif
    return true;
}

x86::reg32 File::remove(const char *filename)
{
    std::string f = moveToRoot(filename);
    if (!writable(f))
    {
        pf::logError("unable to delete file %s: it is in the read-only CD folder", filename);
        return 0;
    }
    dropParked();
    if (unlink(f.c_str()) != 0)
    {
        pf::logError("unable to delete file %s", filename);
        logSave(filename, "delete %s failed (errno %d)", f.c_str(), errno);
        return 0;
    }
    else
    {
        pf::logDebug("Deleting file %s", filename);
        logSave(filename, "delete %s", f.c_str());
        return 1;
    }
}

}
