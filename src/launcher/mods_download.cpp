// A mod's files from its archive, each written next to its final place (the Vita cannot create files deep in a
// folder tree); a failure or cancel restores the game folder.

#include "mods_download.h"
#include "mods_registry.h"
#include "net.h"
#include "unpack.h"
#include <platform/log.h>
#include <platform/sha256.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef __vita__
#include <psp2/io/devctl.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <sys/statvfs.h>
#include <thread>
#endif

namespace ug2 { namespace launcher {

namespace
{

using Phase = cfgui::ModFiles::Phase;

const int Attempts = 4;                 // a dropped connection resumes with Range this many times in all
const size_t HashBytes = 512 * 1024;
const uint64_t Megabyte = 1000000;

bool fileSize(const std::string& path, uint64_t* bytes)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    *bytes = uint64_t(info.st_size);
    return true;
}

std::string wholeMegabytes(uint64_t bytes)
{
    return std::to_string((bytes + Megabyte - 1) / Megabyte) + " MB";
}

}

int folderDepth(const std::string& folder)
{
    const size_t colon = folder.find(':');
    const std::string path = colon == std::string::npos ? folder : folder.substr(colon + 1);
    int depth = 0;
    for (size_t start = 0; start < path.size();)
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        depth += end > start;
        start = end + 1;
    }
    return depth;
}

bool freeSpace(const std::string& folder, uint64_t* bytes)
{
#ifdef __vita__
    const size_t colon = folder.find(':');
    if (colon == std::string::npos)
        return false;
    SceIoDevInfo info = {};
    if (sceIoDevctl(folder.substr(0, colon + 1).c_str(), 0x3001, nullptr, 0, &info, sizeof(info)) < 0)
        return false;
    *bytes = uint64_t(info.free_size);
    return true;
#else
    struct statvfs info;
    if (statvfs(folder.c_str(), &info) != 0)
        return false;
    *bytes = uint64_t(info.f_bavail) * info.f_frsize;
    return true;
#endif
}

struct ModDownloads::Job
{
    std::string gameFolder;
    std::string caFile;
    std::string name;
    const modreq::Setting* setting = nullptr;
    mods::Download download;
    int maxDepth = 0;                   // folder levels below the game folder

    std::atomic<bool> abort{ false };
    std::atomic<int> phase{ int(Phase::Download) };
    std::atomic<uint64_t> downloaded{ 0 };
    std::atomic<uint64_t> unpacked{ 0 };
    std::atomic<int> installed{ 0 };
    std::string error;                  // set before phase becomes Failed
    std::string detail;                 // for the log

    std::string archive;
    std::vector<std::string> folders;   // made by the job, outermost first
    std::vector<std::string> parts;
    std::vector<std::string> placed;
    std::vector<std::string> backups;   // files the install replaced, kept as name.old until it ends

#ifdef __vita__
    SceUID thread = -1;
    Job* self = this;

    static int entry(SceSize, void* argument)
    {
        (*static_cast<Job* const*>(argument))->run();
        return 0;
    }
#else
    std::thread thread;
#endif

    bool fail(const std::string& message, const std::string& why = std::string())
    {
        if (!abort.load())
        {
            error = message;
            detail = why.empty() ? message : message + " (" + why + ")";
        }
        return false;
    }

    std::string target(const mods::FileLocation& file) const { return gameFolder + "/" + file.to; }

    bool check()
    {
        if (download.archiveBytes == 0 || download.archiveSha256.size() != 64)
            return fail("The mod's download is not set up correctly.");
        for (const mods::FileLocation& file : download.files)
        {
            if (!unpack::safeName(file.from))
                return fail("Unsafe archive path: " + file.from);
            if (!unpack::safeName(file.to))
                return fail("Unsafe install path: " + file.to);
            if (folderDepth(file.to) - 1 > maxDepth)
                return fail("Install path too deep for the Vita: " + file.to);
        }
        return true;
    }

    bool fetch()
    {
        archive = gameFolder + "/mod-" + download.archiveSha256.substr(0, 16) + ".download";
        uint64_t have = 0;
        if (fileSize(archive, &have) && have > download.archiveBytes)
        {
            remove(archive.c_str());
            have = 0;
        }
        downloaded = have;
        if (have < download.archiveBytes)
        {
            std::string why;
            if (!net::start(caFile.empty() ? nullptr : caFile.c_str(), &why))
            {
                net::stop();
                return fail("No network connection.", why);
            }
            net::Result result = net::Result::Failed;
            for (int attempt = 0; attempt < Attempts && !abort.load(); ++attempt)
            {
                const uint64_t before = have;
                result = net::download(download.url, archive, &have, download.archiveBytes, &abort, &downloaded, &why);
                if (result != net::Result::Failed || have <= before)
                    break;
            }
            net::stop();
            if (result == net::Result::Cancelled || abort.load())
                return false;
            if (result != net::Result::Ok)
                return fail("The download failed.", why);
        }
        if (have != download.archiveBytes)
            return fail("The download has the wrong size.", std::to_string(have) + " bytes");
        FILE* in = fopen(archive.c_str(), "rb");
        char* block = static_cast<char*>(malloc(HashBytes));
        if (!in || !block)
        {
            if (in)
                fclose(in);
            free(block);
            return fail("Cannot read the download.");
        }
        pf::Sha256 hash;
        size_t n;
        while (!abort.load() && (n = fread(block, 1, HashBytes, in)) > 0)
            hash.update(reinterpret_cast<const uint8_t*>(block), n);
        const bool readError = ferror(in) != 0;
        fclose(in);
        free(block);
        if (abort.load())
            return false;
        if (readError)
            return fail("Cannot read the download.");
        if (hash.hex() != download.archiveSha256)
            return fail("The download is damaged (SHA-256 mismatch).");
        return true;
    }

    void makeFolders(const std::string& path)
    {
        for (size_t at = path.find('/', gameFolder.size() + 1); at != std::string::npos; at = path.find('/', at + 1))
        {
            const std::string folder = path.substr(0, at);
            if (mkdir(folder.c_str(), 0777) == 0)
                folders.push_back(folder);
        }
    }

    bool unpackFiles()
    {
        phase = int(Phase::Unpack);
        std::vector<unpack::Entry> entries;
        for (const mods::FileLocation& file : download.files)
        {
            makeFolders(target(file));
            parts.push_back(target(file) + ".part");
            entries.push_back(unpack::Entry{ file.from, parts.back(), file.bytes });
        }
        std::string why;
        if (!unpack::extract(archive, entries, abort, &unpacked, &why))
            return fail(why);
        return !abort.load();
    }

    bool install()
    {
        phase = int(Phase::Install);
        for (const mods::FileLocation& file : download.files)
        {
            if (abort.load())
                return false;
            const std::string path = target(file);
            uint64_t bytes = 0;
            if (fileSize(path, &bytes))
            {
                const std::string old = path + ".old";
                remove(old.c_str());
                if (rename(path.c_str(), old.c_str()) != 0)
                    return fail("Cannot replace " + file.to);
                backups.push_back(path);
            }
            if (rename((path + ".part").c_str(), path.c_str()) != 0)
                return fail("Cannot install " + file.to);
            placed.push_back(path);
            if (!fileSize(path, &bytes) || bytes != file.bytes)
                return fail("Wrong size after installing " + file.to);
            ++installed;
        }
        if (setting && !setting->requirementsMet(gameFolder))
            return fail("The installed files do not meet the mod's requirements.");
        return !abort.load();
    }

    void undo()
    {
        for (const std::string& part : parts)
            remove(part.c_str());
        for (auto it = placed.rbegin(); it != placed.rend(); ++it)
            remove(it->c_str());
        for (const std::string& path : backups)
            rename((path + ".old").c_str(), path.c_str());
        if (!archive.empty())
            remove(archive.c_str());
        for (auto it = folders.rbegin(); it != folders.rend(); ++it)
            rmdir(it->c_str());
    }

    void run()
    {
        if (check() && fetch() && unpackFiles() && install())
        {
            for (const std::string& path : backups)
                remove((path + ".old").c_str());
            remove(archive.c_str());
            phase = int(Phase::Done);
            return;
        }
        undo();
        if (abort.load())
            phase = int(Phase::Cancelled);
        else
        {
            if (error.empty())
                fail("The files could not be installed.");
            phase = int(Phase::Failed);
        }
    }
};

ModDownloads::ModDownloads(const Options& options)
    :   m_options(options)
{
}

ModDownloads::~ModDownloads()
{
    if (m_job)
    {
        cancel();
        finish();
    }
}

bool ModDownloads::requirementsMet(int id)
{
    return mods::def(mods::get(mods::Id(id))).requirementsMet(m_options.gameFolder);
}

bool ModDownloads::start(int id, std::string* error)
{
    const mods::Mod& mod = mods::get(mods::Id(id));
    const mods::Source& source = mods::source(mod.source);
    if (m_job || !source.downloadable())
    {
        *error = "The files for this mod cannot be downloaded.";
        return false;
    }
    if (m_options.lock)
        m_options.lock->take();
    const uint64_t needed = source.download.spaceNeeded();
    uint64_t free = 0;
    if (m_options.freeSpace && m_options.freeSpace(m_options.gameFolder, &free) && free < needed)
    {
        *error = "Not enough free space: " + wholeMegabytes(needed) + " needed, " + std::to_string(free / Megabyte) +
                 " MB free.";
        pf::logError("mods: %s: %s", source.mod, error->c_str());
        if (m_options.lock)
            m_options.lock->release();
        return false;
    }
    m_job = new Job;
    m_job->gameFolder = m_options.gameFolder;
    m_job->caFile = m_options.caFile;
    m_job->name = source.mod;
    m_job->setting = &mods::def(mod);
    m_job->download = source.download;
    m_job->maxDepth = m_options.maxFolderDepth - folderDepth(m_options.gameFolder);
    m_shown = 0.0f;
    pf::logInfo("mods: fetching the files for %s from %s (%llu bytes)", source.mod, source.download.url.c_str(),
                (unsigned long long)source.download.archiveBytes);
#ifdef __vita__
    m_job->thread = sceKernelCreateThread("mods_download", Job::entry, 0x10000100, 512 * 1024, 0, 0, nullptr);
    if (m_job->thread < 0 || sceKernelStartThread(m_job->thread, sizeof(Job*), &m_job->self) < 0)
    {
        pf::logError("mods: the download thread did not start (0x%08x)", unsigned(m_job->thread));
        if (m_job->thread >= 0)
            sceKernelDeleteThread(m_job->thread);
        delete m_job;
        m_job = nullptr;
        if (m_options.lock)
            m_options.lock->release();
        *error = "The download could not be started.";
        return false;
    }
#else
    m_job->thread = std::thread([job = m_job] { job->run(); });
#endif
    return true;
}

cfgui::ModFiles::Progress ModDownloads::progress()
{
    Progress p;
    if (!m_job)
        return p;
    const Job& job = *m_job;
    p.phase = Phase(job.phase.load());
    p.total = job.download.archiveBytes;
    p.downloaded = std::min(job.downloaded.load(), p.total);
    const uint64_t unpackBytes = job.download.installedBytes();
    const size_t files = job.download.files.size();
    float fraction = 0.0f;
    switch (p.phase)
    {
    case Phase::Download:
        fraction = p.total ? 0.5f * float(double(p.downloaded) / double(p.total)) : 0.0f;
        break;
    case Phase::Unpack:
        fraction = 0.5f + (unpackBytes ? 0.4f * float(double(std::min(job.unpacked.load(), unpackBytes)) /
                                                      double(unpackBytes)) : 0.0f);
        break;
    case Phase::Install:
        fraction = 0.9f + (files ? 0.1f * float(job.installed.load()) / float(files) : 0.0f);
        break;
    case Phase::Done:
        fraction = 1.0f;
        break;
    case Phase::Failed:
        p.error = job.error;
        fraction = m_shown;
        break;
    case Phase::Cancelled:
        fraction = m_shown;
        break;
    }
    m_shown = std::max(m_shown, std::min(1.0f, fraction));
    p.fraction = m_shown;
    return p;
}

void ModDownloads::cancel()
{
    if (m_job)
        m_job->abort = true;
}

void ModDownloads::finish()
{
    if (!m_job)
        return;
#ifdef __vita__
    sceKernelWaitThreadEnd(m_job->thread, nullptr, nullptr);
    sceKernelDeleteThread(m_job->thread);
#else
    m_job->thread.join();
#endif
    const Phase phase = Phase(m_job->phase.load());
    if (phase == Phase::Done)
        pf::logInfo("mods: the files for %s are installed", m_job->name.c_str());
    else if (phase == Phase::Cancelled)
        pf::logInfo("mods: fetching the files for %s was cancelled", m_job->name.c_str());
    else
        pf::logError("mods: fetching the files for %s failed: %s", m_job->name.c_str(), m_job->detail.c_str());
    delete m_job;
    m_job = nullptr;
    if (m_options.lock)
        m_options.lock->release();
}

}}
