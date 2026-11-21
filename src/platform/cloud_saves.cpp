// The cloud saves (include/platform/cloud_saves.h): the browser version's /vault protocol on the Vita.
//
// The server keeps one sealed box per passphrase (web/src/app/vault.ts): the passphrase's PBKDF2-SHA-256 (100,000
// rounds, a fixed salt) gives 64 bytes, the first 32 an AES-256-GCM key and the second 32 the token that names the
// box (Bearer, base64url). A box is a version byte (1), a 12-byte IV and the ciphertext with its 16-byte tag; its
// plain text is a raw deflate stream of a zip of the save folder's files (platform/vault_zip.h). A PUT with If-Match
// (the version it follows) or If-None-Match: * (a new passphrase) saves it; a 412 answers with the version that is there.

#include <platform/cloud_saves.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sha256.h>
#include <platform/tls.h>
#include <winapi/ws2_32.h>
#include "vault_zip.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>

#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <ctime>
#include <map>

namespace pf { namespace cloud
{

namespace
{

using vaultzip::Bytes;
using vaultzip::Entry;

const char* const Profiles = "NFS Underground 2";     // what the game makes under the save folder (savefs.ts PROFILES_DIR)
const char Salt[] = "nfsu2-xyz keyphrase saves v1";    // the key's salt and the box's associated data
const size_t MaxBox = 256 << 10;                       // what the server takes
const size_t MaxPlain = 64 << 20;
const uint32_t RequestMs = 30000;

std::string s_gameFolder;
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
std::string s_status = "Not used yet";
unsigned s_version = 0;
bool s_busy = false;

void setStatus(const std::string& text)
{
    pthread_mutex_lock(&s_lock);
    s_status = text;
    ++s_version;
    pthread_mutex_unlock(&s_lock);
    pf::logInfo("cloud saves: %s", text.c_str());
}

// Why the last thing failed, in a few words (the status). One action runs at a time, and no function throws.
std::string s_failure;

bool fail(const std::string& text)
{
    s_failure = text;
    return false;
}

uint64_t nowMs()
{
    return sceKernelGetProcessTimeWide() / 1000;
}

// ---- The passphrase's keys -----------------------------------------------------------------------------------------------

struct Keys
{
    uint8_t     key[32];
    std::string token;
};

// The 8 words of the passphrase after "nfsu2-xyz", in lower case, as web/src/app/vault.ts readPhrase takes them.
bool readPhrase(const std::string& text, std::string* phrase)
{
    std::vector<std::string> words;
    std::string w;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        const char c = i < text.size() ? char(tolower(static_cast<unsigned char>(text[i]))) : 0;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            w += c;
        else if (!w.empty())
        {
            words.push_back(w);
            w.clear();
        }
    }
    const size_t first = words.size() >= 2 && words[0] == "nfsu2" && words[1] == "xyz" ? 2 : 0;
    if (words.size() - first != 8)
        return false;
    *phrase = "nfsu2-xyz";
    for (size_t i = first; i < words.size(); ++i)
        *phrase += "-" + words[i];
    return true;
}

std::string base64Url(const uint8_t* data, size_t size)
{
    uint8_t out[96];
    size_t n = 0;
    mbedtls_base64_encode(out, sizeof(out), &n, data, size);
    std::string text;
    for (size_t i = 0; i < n; ++i)
    {
        if (out[i] == '+')
            text += '-';
        else if (out[i] == '/')
            text += '_';
        else if (out[i] != '=')
            text += char(out[i]);
    }
    return text;
}

bool keysOf(const std::string& phrase, Keys* keys)
{
    uint8_t bits[64];
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size(),
                                      reinterpret_cast<const uint8_t*>(Salt), sizeof(Salt) - 1, 100000, sizeof(bits), bits) != 0)
        return fail("Key failed");
    memcpy(keys->key, bits, 32);
    keys->token = base64Url(bits + 32, 32);
    memset(bits, 0, sizeof(bits));
    return true;
}

bool seal(const Keys& keys, const Bytes& plain, Bytes* sealed)
{
    Bytes& box = *sealed;
    box.assign(1 + 12 + plain.size() + 16, 0);
    box[0] = 1;
    sceKernelGetRandomNumber(&box[1], 12);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, keys.key, 256);
    if (result == 0)
        result = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain.size(), &box[1], 12,
                                           reinterpret_cast<const uint8_t*>(Salt), sizeof(Salt) - 1, plain.data(),
                                           &box[13], 16, &box[13 + plain.size()]);
    mbedtls_gcm_free(&gcm);
    return result == 0 || fail("Seal failed");
}

bool unseal(const Keys& keys, const Bytes& box, Bytes* opened)
{
    if (box.size() < 1 + 12 + 16 || box[0] != 1)
        return fail("Unknown version");
    const size_t size = box.size() - 29;
    Bytes& plain = *opened;
    plain.assign(size, 0);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, keys.key, 256);
    if (result == 0)
        result = mbedtls_gcm_auth_decrypt(&gcm, size, &box[1], 12, reinterpret_cast<const uint8_t*>(Salt), sizeof(Salt) - 1,
                                          &box[13 + size], 16, &box[13], plain.data());
    mbedtls_gcm_free(&gcm);
    return result == 0 || fail("Wrong passphrase");
}

// ---- HTTPS ------------------------------------------------------------------------------------------------------------------

struct Response
{
    int         status = 0;
    std::string etag;
    long long   saved = 0;
    Bytes       body;
};

bool startsWith(const std::string& line, const char* name, std::string* value)
{
    const size_t n = strlen(name);
    if (line.size() < n + 1 || strncasecmp(line.c_str(), name, n) != 0 || line[n] != ':')
        return false;
    size_t i = n + 1;
    while (i < line.size() && line[i] == ' ')
        ++i;
    *value = line.substr(i);
    return true;
}

bool request(const std::string& host, const char* method, const Keys& keys,
             const std::vector<std::pair<std::string, std::string>>& headers, const Bytes* body, Response* result)
{
    if (!win32::ws2_32::netStart())
        return fail("No network");
    tls::Stream stream;
    std::string why;
    if (!stream.open(host.c_str(), 443, 15000, why))
    {
        pf::logError("cloud saves: %s did not open: %s", host.c_str(), why.c_str());
        return fail("No connection");
    }
    std::string head = std::string(method) + " /vault HTTP/1.1\r\nHost: " + host + "\r\nAuthorization: Bearer " +
                       keys.token + "\r\nUser-Agent: NFSU2VITA/" NFSU_VERSION "\r\nAccept: */*\r\nConnection: close\r\n";
    for (const auto& h : headers)
        head += h.first + ": " + h.second + "\r\n";
    if (body)
        head += "Content-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body->size()) + "\r\n";
    head += "\r\n";
    if (!stream.write(head.data(), head.size(), RequestMs) || (body && !stream.write(body->data(), body->size(), RequestMs)))
        return fail("No connection");
    Response& r = *result;
    r = Response();
    std::string raw;
    uint8_t buffer[4096];
    const uint64_t start = nowMs();
    size_t bodyAt = std::string::npos;
    long long length = -1;
    bool chunked = false, open = true;
    for (;;)
    {
        if (nowMs() - start > RequestMs)
            return fail("No answer");
        if (open)
        {
            if (!stream.waitReadable(500))
                continue;
            const int n = stream.read(buffer, sizeof(buffer));
            if (n < 0)
                open = false;
            else
                raw.append(reinterpret_cast<char*>(buffer), size_t(n));
        }
        if (bodyAt == std::string::npos)
        {
            const size_t end = raw.find("\r\n\r\n");
            if (end == std::string::npos)
            {
                if (!open)
                    return fail("No answer");
                continue;
            }
            bodyAt = end + 4;
            size_t pos = 0;
            std::string line;
            bool first = true;
            while (pos < end + 2)
            {
                const size_t eol = raw.find("\r\n", pos);
                line = raw.substr(pos, eol - pos);
                pos = eol + 2;
                std::string value;
                if (first)
                {
                    r.status = line.size() > 12 ? atoi(line.c_str() + 9) : 0;
                    first = false;
                }
                else if (startsWith(line, "ETag", &value))
                    r.etag = value;
                else if (startsWith(line, "X-Vault-Saved", &value))
                    r.saved = atoll(value.c_str());
                else if (startsWith(line, "Content-Length", &value))
                    length = atoll(value.c_str());
                else if (startsWith(line, "Transfer-Encoding", &value))
                    chunked = strncasecmp(value.c_str(), "chunked", 7) == 0;
            }
            if (r.status == 304 || r.status == 204 || r.status == 404 || r.status == 412)
                length = length < 0 ? 0 : length;
        }
        const size_t have = raw.size() - bodyAt;
        if (chunked)
        {
            // The chunks so far, whole: done at the last (empty) one.
            Bytes out;
            size_t pos = bodyAt;
            bool done = false;
            for (;;)
            {
                const size_t eol = raw.find("\r\n", pos);
                if (eol == std::string::npos)
                    break;
                const size_t n = strtoul(raw.c_str() + pos, nullptr, 16);
                if (n == 0)
                {
                    done = true;
                    break;
                }
                if (eol + 2 + n + 2 > raw.size())
                    break;
                out.insert(out.end(), raw.begin() + long(eol + 2), raw.begin() + long(eol + 2 + n));
                pos = eol + 2 + n + 2;
            }
            if (done)
            {
                r.body = out;
                return true;
            }
        }
        else if (length >= 0 && have >= size_t(length))
        {
            r.body.assign(raw.begin() + long(bodyAt), raw.begin() + long(bodyAt + size_t(length)));
            return true;
        }
        else if (!open)
        {
            if (length >= 0)
                return fail("No answer");
            r.body.assign(raw.begin() + long(bodyAt), raw.end());
            return true;
        }
        if (!open)
            return fail("No answer");
    }
}

// ---- The saves here ---------------------------------------------------------------------------------------------------

std::string saveRoot()
{
    std::string root = Config::getString("save_dir", "");
    if (root.empty())
        root = s_gameFolder + "/save";
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    return root;
}

std::string dataPath(const char* name)
{
    return s_gameFolder + "/" + name;
}

struct Local
{
    std::vector<Entry> files;       // path from the save folder, forward slashes, in path order
    uint64_t           newest = 0;  // the newest file's time
};

bool readFile(const std::string& path, Bytes* data)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    data->clear();
    uint8_t buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
        data->insert(data->end(), buffer, buffer + n);
    fclose(f);
    return true;
}

// A DVD or Magazine cover's folder ("<NAME> DVD 1"): those stay in the browser that took them.
bool coverFolder(const std::string& name)
{
    for (const char* kind : { " DVD ", " Magazine " })
    {
        const size_t at = name.rfind(kind);
        if (at != std::string::npos && at > 0 && isdigit(static_cast<unsigned char>(name[at + strlen(kind)])))
            return true;
    }
    return false;
}

// Whether a cloud save keeps the file: a profile's files, not a cover's, a partial write or a screenshot.
bool kept(const std::string& path)
{
    const std::string prefix = std::string(Profiles) + "/";
    if (strncasecmp(path.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    const size_t slash = path.find('/', prefix.size());
    if (slash == std::string::npos || coverFolder(path.substr(prefix.size(), slash - prefix.size())))
        return false;
    const size_t n = path.size();
    return !(n > 5 && strcasecmp(path.c_str() + n - 5, ".part") == 0) && !(n > 9 && strcasecmp(path.c_str() + n - 9, ".incoming") == 0);
}

void walk(const std::string& root, const std::string& below, Local* local)
{
    SceUID dir = sceIoDopen((root + (below.empty() ? "" : "/" + below)).c_str());
    if (dir < 0)
        return;
    SceIoDirent entry;
    memset(&entry, 0, sizeof(entry));
    std::vector<std::pair<std::string, bool>> names;
    while (sceIoDread(dir, &entry) > 0)
    {
        names.push_back({ entry.d_name, SCE_S_ISDIR(entry.d_stat.st_mode) != 0 });
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dir);
    std::sort(names.begin(), names.end());
    for (const auto& n : names)
    {
        if (n.first == "." || n.first == ".." || n.first[0] == '.')
            continue;
        const std::string path = below.empty() ? n.first : below + "/" + n.first;
        if (n.second)
        {
            walk(root, path, local);
            continue;
        }
        if (!kept(path))
            continue;
        Entry e;
        e.path = path;
        if (!readFile(root + "/" + path, &e.data))
            continue;
        struct stat status;
        if (stat((root + "/" + path).c_str(), &status) == 0 && uint64_t(status.st_mtime) > local->newest)
            local->newest = uint64_t(status.st_mtime);
        local->files.push_back(e);
    }
}

Local readLocal()
{
    Local local;
    walk(saveRoot(), "", &local);
    return local;
}

// What the files hold: SHA-256 over their paths and bytes, in path order.
std::string hashOf(const std::vector<Entry>& files)
{
    Bytes all;
    for (const Entry& e : files)
    {
        const uint32_t n = uint32_t(e.path.size()), m = uint32_t(e.data.size());
        for (uint32_t v : { n, m })
            for (int k = 3; k >= 0; --k)
                all.push_back(uint8_t(v >> (k * 8)));
        all.insert(all.end(), e.path.begin(), e.path.end());
        all.insert(all.end(), e.data.begin(), e.data.end());
    }
    return sha256Hex(all.data(), all.size());
}

bool profilesIn(const std::vector<Entry>& files)
{
    return !files.empty();
}

void makeFolders(const std::string& path)
{
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/')
        {
            const std::string folder = path.substr(0, i);
            if (folder.back() != ':')
                sceIoMkdir(folder.c_str(), 0777);
        }
}

bool writeFile(const std::string& path, const Bytes& data)
{
    makeFolders(path);
    const std::string part = path + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f)
        return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok)
    {
        unlink(part.c_str());
        return false;
    }
    if (rename(part.c_str(), path.c_str()) == 0)
        return true;
    struct stat status;
    return stat(path.c_str(), &status) == 0 && unlink(path.c_str()) == 0 && rename(part.c_str(), path.c_str()) == 0;
}

// A zip of the saves here beside the game's save folder before they are replaced; the last 5 stay.
bool backUp(const Local& local)
{
    if (local.files.empty())
        return true;
    const std::string folder = dataPath("save_backups");
    sceIoMkdir(folder.c_str(), 0777);
    char name[64];
    snprintf(name, sizeof(name), "/saves-%llu.zip", (unsigned long long)std::time(nullptr));
    if (!writeFile(folder + name, vaultzip::zip(local.files)))
        return fail("Backup failed");
    // Older backups: the names sort by time.
    std::vector<std::string> names;
    SceUID dir = sceIoDopen(folder.c_str());
    if (dir >= 0)
    {
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0)
        {
            if (strncmp(entry.d_name, "saves-", 6) == 0)
                names.push_back(entry.d_name);
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + 5 < names.size(); ++i)
        unlink((folder + "/" + names[i]).c_str());
    return true;
}

// A safe path below the save folder from a zip's, or empty.
std::string safePath(const std::string& path)
{
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return std::string();
    size_t from = 0;
    while (from <= path.size())
    {
        size_t to = path.find('/', from);
        if (to == std::string::npos)
            to = path.size();
        const std::string part = path.substr(from, to - from);
        if (part.empty() || part == "." || part == "..")
            return std::string();
        from = to + 1;
    }
    return path;
}

// The cloud's saves in place of the ones here, which a backup holds first.
bool restore(const std::vector<Entry>& theirs, const Local& local)
{
    std::vector<Entry> take;
    for (const Entry& e : theirs)
    {
        const std::string path = safePath(e.path);
        if (!path.empty() && kept(path))
        {
            Entry t;
            t.path = path;
            t.data = e.data;
            take.push_back(t);
        }
    }
    if (take.empty())
        return fail("Cloud has no saves");
    if (!backUp(local))
        return false;
    const std::string root = saveRoot();
    std::map<std::string, bool> theirsHas;
    for (const Entry& e : take)
    {
        theirsHas[e.path] = true;
        if (!writeFile(root + "/" + e.path, e.data))
            return fail("Write failed");
    }
    // What the cloud does not have goes, as the browser's restore does (the backup has it).
    for (const Entry& e : local.files)
        if (!theirsHas.count(e.path))
            unlink((root + "/" + e.path).c_str());
    return true;
}

// ---- The link ---------------------------------------------------------------------------------------------------------------

// The version this Vita sent or took last (the server's ETag) and what the saves held then.
struct Link
{
    std::string rev, hash;
    bool        known = false;
};

Link readLink()
{
    Link link;
    FILE* f = fopen(dataPath("cloud_saves.txt").c_str(), "r");
    if (!f)
        return link;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (text.compare(0, 4, "rev=") == 0)
            link.rev = text.substr(4);
        else if (text.compare(0, 5, "hash=") == 0)
            link.hash = text.substr(5);
    }
    fclose(f);
    link.known = !link.rev.empty() && !link.hash.empty();
    return link;
}

void writeLink(const std::string& rev, const std::string& hash)
{
    const std::string text = "rev=" + rev + "\nhash=" + hash + "\n";
    writeFile(dataPath("cloud_saves.txt"), Bytes(text.begin(), text.end()));
}

// ---- The actions ------------------------------------------------------------------------------------------------------------

struct Session
{
    std::string host;
    Keys        keys;
};

bool open(Session* s)
{
    std::string phrase;
    const std::string text = Config::getString("revived_passphrase", "");
    if (text.empty())
        return fail("Set your passphrase");
    if (!readPhrase(text, &phrase))
        return fail("Not a passphrase");
    s->host = Config::getString("revived_server", "nfsu2.xyz");
    return keysOf(phrase, &s->keys);
}

enum Put { Sent, Other, Failed };

// The saves here sealed and sent over `rev` (empty: as new). Other: another version is there, in *otherRev.
Put put(const Session& s, const Local& local, const std::string& rev, std::string* now, std::string* otherRev)
{
    if (!profilesIn(local.files))
        return fail("No saves here"), Failed;
    Bytes box;
    if (!seal(s.keys, vaultzip::deflateStored(vaultzip::zip(local.files)), &box))
        return Failed;
    if (box.size() > MaxBox)
        return fail("Saves too large"), Failed;
    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back(rev.empty() ? std::make_pair(std::string("If-None-Match"), std::string("*"))
                                  : std::make_pair(std::string("If-Match"), rev));
    Response r;
    if (!request(s.host, "PUT", s.keys, headers, &box, &r))
        return Failed;
    if (r.status == 412)
    {
        *otherRev = r.etag;
        return Other;
    }
    if (r.status < 200 || r.status > 299 || r.etag.empty())
        return fail(r.status == 429 ? "Wait a minute" : r.status == 413 ? "Saves too large" : "Server error " + std::to_string(r.status)), Failed;
    *now = r.etag;
    return Sent;
}

enum Get { Got, None, Same, Error };

// The cloud's version: its saves and rev; None when there is no version, Same when it is still `rev`.
Get get(const Session& s, const std::string& rev, std::vector<Entry>* files, std::string* now)
{
    std::vector<std::pair<std::string, std::string>> headers;
    if (!rev.empty())
        headers.push_back({ "If-None-Match", rev });
    Response r;
    if (!request(s.host, "GET", s.keys, headers, nullptr, &r))
        return Error;
    if (r.status == 404)
        return None;
    if (r.status == 304)
        return Same;
    if (r.status != 200 || r.etag.empty())
        return fail(r.status == 401 ? "Wrong passphrase" : r.status == 429 ? "Wait a minute" : "Server error " + std::to_string(r.status)), Error;
    Bytes packed, zipBytes;
    if (!unseal(s.keys, r.body, &packed))
        return Error;
    if (!vaultzip::inflate(packed.data(), packed.size(), &zipBytes, MaxPlain) || !vaultzip::unzip(zipBytes, files, MaxPlain))
        return fail("Cloud saves damaged"), Error;
    *now = r.etag;
    return Got;
}

bool upload(const Session& s)
{
    const Local local = readLocal();
    const Link link = readLink();
    std::string now, other;
    Put sent = put(s, local, link.known ? link.rev : std::string(), &now, &other);
    if (sent == Other)
        sent = other.empty() ? Failed : put(s, local, other, &now, &other);     // this one replaces it, as asked
    if (sent == Other)
        return fail("Cloud changed, try again");
    if (sent == Failed)
        return false;
    writeLink(now, hashOf(local.files));
    setStatus("Uploaded");
    return true;
}

bool download(const Session& s)
{
    std::vector<Entry> files;
    std::string now;
    const Get got = get(s, std::string(), &files, &now);
    if (got == None)
        return fail("Cloud has no saves");
    if (got != Got)
        return false;
    if (!restore(files, readLocal()))
        return false;
    writeLink(now, hashOf(readLocal().files));
    setStatus("Downloaded");
    return true;
}

bool sync(const Session& s)
{
    const Local local = readLocal();
    const Link link = readLink();
    const std::string hash = hashOf(local.files);
    std::vector<Entry> files;
    std::string now, other;
    const Get got = get(s, link.known ? link.rev : std::string(), &files, &now);
    if (got == Error)
        return false;
    if (got == None)
    {
        // Nothing in the cloud yet: the saves here go up.
        const Put sent = put(s, local, std::string(), &now, &other);
        if (sent == Other)
            return fail("Cloud changed, try again");
        if (sent == Failed)
            return false;
        writeLink(now, hash);
        setStatus("Uploaded");
        return true;
    }
    const bool changedHere = !link.known || hash != link.hash;
    if (got == Same)
    {
        if (!changedHere)
        {
            setStatus("Already in sync");
            return true;
        }
        const Put sent = put(s, local, link.rev, &now, &other);
        if (sent == Other)
            return fail("Cloud changed, try again");
        if (sent == Failed)
            return false;
        writeLink(now, hash);
        setStatus("Uploaded");
        return true;
    }
    // The cloud changed since this Vita's last sync (or there was none).
    if (!local.files.empty() && changedHere)
        return fail("Both changed: pick");
    if (!restore(files, local))
        return false;
    writeLink(now, hashOf(readLocal().files));
    setStatus("Downloaded");
    return true;
}

struct Job
{
    Action action;
};

void* worker(void* argument)
{
    const Job job = *static_cast<Job*>(argument);
    delete static_cast<Job*>(argument);
    Session session;
    bool ok = open(&session);
    if (ok)
        ok = job.action == Action::Sync ? sync(session) : job.action == Action::Upload ? upload(session) : download(session);
    if (!ok)
        setStatus(s_failure);
    pthread_mutex_lock(&s_lock);
    s_busy = false;
    ++s_version;
    pthread_mutex_unlock(&s_lock);
    return nullptr;
}

}

void configure(const std::string& gameFolder)
{
    s_gameFolder = gameFolder;
    while (s_gameFolder.size() > 1 && s_gameFolder.back() == '/')
        s_gameFolder.pop_back();
}

bool start(Action action)
{
    pthread_mutex_lock(&s_lock);
    if (s_busy)
    {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    s_busy = true;
    pthread_mutex_unlock(&s_lock);
    setStatus("Working...");
    Job* job = new Job{ action };
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 512 * 1024);     // the TLS handshake's and the save files'
    const bool ok = pthread_create(&thread, &attributes, worker, job) == 0;
    pthread_attr_destroy(&attributes);
    if (!ok)
    {
        delete job;
        pthread_mutex_lock(&s_lock);
        s_busy = false;
        pthread_mutex_unlock(&s_lock);
        setStatus("Could not start");
        return false;
    }
    pthread_detach(thread);
    return true;
}

bool busy()
{
    pthread_mutex_lock(&s_lock);
    const bool b = s_busy;
    pthread_mutex_unlock(&s_lock);
    return b;
}

unsigned version()
{
    pthread_mutex_lock(&s_lock);
    const unsigned v = s_version;
    pthread_mutex_unlock(&s_lock);
    return v;
}

std::string status()
{
    pthread_mutex_lock(&s_lock);
    const std::string s = s_status;
    pthread_mutex_unlock(&s_lock);
    return s;
}

}}
