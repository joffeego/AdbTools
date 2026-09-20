#include "core/fileio.h"

#include <filesystem>
#include <fstream>

namespace adb::core {

bool writeFileAtomic(const std::string& path, const std::string& contents) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    const fs::path parent = target.parent_path();
    const fs::path temp = parent / (target.filename().string() + ".tmp");

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) {  // disk full / write error: do not touch the real file
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }

    fs::rename(temp, target, ec);
    if (ec) {
        // Some filesystems refuse to replace an existing file; retry after
        // removing it, but only now that the complete new file exists.
        ec.clear();
        fs::remove(target, ec);
        ec.clear();
        fs::rename(temp, target, ec);
        if (ec) {
            fs::remove(temp, ec);
            return false;
        }
    }
    return true;
}

}  // namespace adb::core
