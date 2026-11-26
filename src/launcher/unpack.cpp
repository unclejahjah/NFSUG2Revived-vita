// libarchive reads the archive; the listed entries are written with large stdio blocks (archive_write_disk is not
// used, it does not suit the Vita).

#include "unpack.h"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ug2 { namespace launcher { namespace unpack {

namespace
{

const size_t ReadBytes = 256 * 1024;
const size_t BlockBytes = 512 * 1024;

std::string entryName(archive_entry* e)
{
    const char* utf8 = archive_entry_pathname_utf8(e);
    const char* raw = archive_entry_pathname(e);
    std::string name = utf8 ? utf8 : raw ? raw : "";
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

bool sameName(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return tolower((unsigned char)x) == tolower((unsigned char)y);
    });
}

std::string archiveError(archive* a, const char* otherwise)
{
    const char* text = archive_error_string(a);
    return text ? text : otherwise;
}

}

bool safeName(const std::string& name)
{
    if (name.empty() || name.size() > 255 || name[0] == '/' || name.find(':') != std::string::npos ||
        name.find('\\') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= name.size())
    {
        size_t end = name.find('/', start);
        if (end == std::string::npos)
            end = name.size();
        const std::string part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        start = end + 1;
    }
    return true;
}

bool extract(const std::string& path, const std::vector<Entry>& entries, const std::atomic<bool>& abort,
             std::atomic<uint64_t>* done, std::string* error)
{
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    if (archive_read_open_filename(a, path.c_str(), ReadBytes) != ARCHIVE_OK)
    {
        *error = "The archive could not be opened: " + archiveError(a, "unknown format");
        archive_read_free(a);
        return false;
    }
    std::vector<bool> written(entries.size(), false);
    char* block = static_cast<char*>(malloc(BlockBytes));
    bool ok = block != nullptr;
    if (!ok)
        *error = "Out of memory while unpacking.";
    archive_entry* e = nullptr;
    int status = ARCHIVE_OK;
    while (ok && (status = archive_read_next_header(a, &e)) == ARCHIVE_OK)
    {
        if (abort.load())
        {
            *error = "cancelled";
            ok = false;
            break;
        }
        const std::string name = entryName(e);
        if (archive_entry_filetype(e) == AE_IFDIR)
            continue;
        std::string trimmed = name;
        while (!trimmed.empty() && trimmed.back() == '/')
            trimmed.pop_back();
        if (!safeName(trimmed))
        {
            *error = "The archive has an unsafe file name: " + name;
            ok = false;
            break;
        }
        size_t index = 0;
        while (index < entries.size() && (written[index] || !sameName(entries[index].from, name)))
            ++index;
        if (index == entries.size() || archive_entry_filetype(e) != AE_IFREG)
            continue;
        const Entry& entry = entries[index];
        FILE* out = fopen(entry.path.c_str(), "wb");
        if (!out)
        {
            *error = "Cannot write " + entry.path;
            ok = false;
            break;
        }
        uint64_t bytes = 0;
        la_ssize_t n = 0;
        bool failed = false;
        while ((n = archive_read_data(a, block, BlockBytes)) > 0)
        {
            if (abort.load())
            {
                *error = "cancelled";
                failed = true;
                break;
            }
            if (fwrite(block, 1, size_t(n), out) != size_t(n))
            {
                *error = "Cannot write " + entry.path + " (is the memory card full?)";
                failed = true;
                break;
            }
            bytes += uint64_t(n);
            if (done)
                *done += uint64_t(n);
        }
        if (fclose(out) != 0 && !failed)
        {
            *error = "Cannot write " + entry.path;
            failed = true;
        }
        if (!failed && n < 0)
        {
            *error = "The archive is damaged: " + archiveError(a, name.c_str());
            failed = true;
        }
        if (!failed && bytes != entry.bytes)
        {
            *error = "Unexpected size of " + entry.from + " in the archive: " + std::to_string(bytes) + " bytes";
            failed = true;
        }
        if (failed)
        {
            remove(entry.path.c_str());
            ok = false;
            break;
        }
        written[index] = true;
    }
    if (ok && status != ARCHIVE_EOF)
    {
        *error = "The archive is damaged: " + archiveError(a, "bad header");
        ok = false;
    }
    for (size_t i = 0; ok && i < entries.size(); ++i)
    {
        if (!written[i])
        {
            *error = "The archive has no " + entries[i].from;
            ok = false;
        }
    }
    free(block);
    archive_read_free(a);
    return ok;
}

}}}
