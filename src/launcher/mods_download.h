#ifndef LAUNCHER_MODS_DOWNLOAD_H_
#define LAUNCHER_MODS_DOWNLOAD_H_

// Fetching a mod's files for the Mods tab: download, SHA-256 check, unpack of the listed entries and install, on a
// worker thread with the file lock held throughout.

#include "cfgui.h"
#include "file_lock.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ug2 { namespace launcher {

const int MaxFolderDepth = 9;       // below ux0:; the Vita cannot create files deeper
// Folder levels below the device ("ux0:" or "/").
int folderDepth(const std::string& folder);
// Free bytes on the folder's device; false when unknown.
bool freeSpace(const std::string& folder, uint64_t* bytes);

class ModDownloads : public cfgui::ModFiles
{
public:
    struct Options
    {
        std::string gameFolder;
        std::string caFile;             // empty: the system's CA store
        FileLock* lock = nullptr;
        std::function<bool(const std::string& folder, uint64_t* bytes)> freeSpace = launcher::freeSpace;
        int maxFolderDepth = MaxFolderDepth;
    };

    explicit ModDownloads(const Options& options);
    ~ModDownloads() override;

    bool requirementsMet(int id) override;
    bool start(int id, std::string* error) override;
    Progress progress() override;
    void cancel() override;
    void finish() override;

    struct Job;

private:
    Options m_options;
    Job* m_job = nullptr;
    float m_shown = 0.0f;               // the bar never moves back
};

}}

#endif
