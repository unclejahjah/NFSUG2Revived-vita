#ifndef UG2_GAME_FOLDER_H_
#define UG2_GAME_FOLDER_H_

#include <sys/stat.h>
#include <string>

// The game folder on the Vita: ux0:data/NeedForSpeedUnderground2, or the short ux0:data/NFSU2 (issue #26).
// The launcher and the game both choose it, by the same rule.

namespace ug2 {

const char LongFolderName[] = "NeedForSpeedUnderground2";
const char ShortFolderName[] = "NFSU2";

inline bool isFolder(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// The long folder if it exists, else an existing short one.
inline std::string findGameFolder(const std::string& root)
{
    const std::string full = root + "/" + LongFolderName, brief = root + "/" + ShortFolderName;
    return !isFolder(full) && isFolder(brief) ? brief : full;
}

// The Vita's game folder, chosen once per process.
inline const std::string& gameFolder()
{
    static const std::string folder = findGameFolder("ux0:data");
    return folder;
}

}

#endif /* !UG2_GAME_FOLDER_H_ */
