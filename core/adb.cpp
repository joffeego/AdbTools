#include "core/adb.h"

#include <sstream>

#include "core/paths.h"
#include "core/strings.h"

namespace adb::core {

const char* const kWriteMarker = "__WRITE__";

std::vector<std::string> devicesArgs() {
    return {"devices", "-l"};
}

std::vector<Device> parseDevices(const std::string& output) {
    std::vector<Device> devices;
    std::istringstream in(output);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line.find("daemon") != std::string::npos) continue;
        if (first && line.rfind("List of devices", 0) == 0) { first = false; continue; }
        first = false;
        std::vector<std::string> tokens = splitWs(line);
        if (tokens.empty() || tokens[0].empty()) continue;
        Device d;
        d.serial = tokens[0];
        d.state = tokens.size() > 1 ? tokens[1] : "";
        // `adb devices -l` appends "key:value" properties; keep the two the UI
        // can show. Plain `adb devices` has none, which is fine.
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const std::string& t = tokens[i];
            if (t.rfind("model:", 0) == 0) {
                d.model = t.substr(6);
            } else if (t.rfind("product:", 0) == 0) {
                d.product = t.substr(8);
            }
        }
        devices.push_back(std::move(d));
    }
    return devices;
}

std::vector<FsEntry> parseLsLa(const std::string& output) {
    std::vector<FsEntry> entries;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("total ", 0) == 0) continue;

        std::vector<std::string> tokens = splitWs(trimmed);
        if (tokens.size() < 6) continue;

        FsEntry e;
        e.perms = tokens[0];
        if (!e.perms.empty()) {
            char c = e.perms[0];
            e.isDir = (c == 'd');
            e.isLink = (c == 'l');
            e.isOther = !(c == '-' || c == 'd' || c == 'l');
        }
        e.size = parseSize(tokens[4]);

        // The name starts after the date columns. `ls -l` shows either
        // "Mon DD HH:MM" / "Mon DD YYYY" (8 metadata fields) or an ISO date
        // "YYYY-MM-DD HH:MM" (7 metadata fields).
        int nameStart = 8;
        if (tokens.size() >= 7 && isMonthName(tokens[5])) {
            nameStart = 8;
        } else {
            nameStart = 7;
        }
        if (nameStart == 8) {
            e.date = tokens[5] + " " + tokens[6] + " " + tokens[7];
        } else {
            e.date = tokens[5] + " " + tokens[6];
        }

        std::string name;
        for (std::size_t i = static_cast<std::size_t>(nameStart); i < tokens.size(); ++i) {
            if (!name.empty()) name += " ";
            name += tokens[i];
        }
        if (e.isLink) {
            std::size_t arrow = name.find(" -> ");
            if (arrow != std::string::npos) {
                e.linkTarget = name.substr(arrow + 4);
                name = name.substr(0, arrow);
            }
        }
        e.name = name;
        if (e.name == "." || e.name == ".." || e.name.empty()) continue;
        entries.push_back(std::move(e));
    }
    return entries;
}

std::string buildListCommand(const std::string& path) {
    const std::string quoted = shellQuote(path);
    return "ls -la " + quoted + " 2>&1; __r=$?; echo " + kWriteMarker + "; "
           "test -w " + quoted + " && echo 1 || echo 0; exit $__r";
}

ListingResult parseListingOutput(const std::string& output) {
    ListingResult result;
    const std::size_t marker = output.find(kWriteMarker);
    const std::string lsOut = marker == std::string::npos ? output : output.substr(0, marker);
    const std::string writeOut =
        marker == std::string::npos ? std::string{} : output.substr(marker + std::string(kWriteMarker).size());
    result.entries = parseLsLa(lsOut);
    result.writable = (trim(writeOut) == "1");
    return result;
}

std::vector<std::string> pullArgs(const std::string& serial, const std::string& remote,
                                  const std::string& localDir) {
    return {"-s", serial, "pull", remote, localDir};
}

std::vector<std::string> pushArgs(const std::string& serial, const std::string& localFile,
                                  const std::string& remoteDir) {
    return {"-s", serial, "push", localFile, remoteDir};
}

std::vector<std::string> deleteArgs(const std::string& serial, const std::string& remote) {
    return {"-s", serial, "shell", "rm", "-rf", shellQuote(remote)};
}

std::vector<std::string> makeDirArgs(const std::string& serial, const std::string& remote) {
    return {"-s", serial, "shell", "mkdir", "-p", shellQuote(remote)};
}

std::vector<std::string> installApkArgs(const std::string& serial, const std::string& apkPath) {
    return {"-s", serial, "install", "-r", apkPath};
}

std::vector<std::string> uninstallArgs(const std::string& serial, const std::string& package) {
    return {"-s", serial, "uninstall", package};
}

std::vector<std::string> clearAppDataArgs(const std::string& serial, const std::string& package) {
    return {"-s", serial, "shell", "pm", "clear", package};
}

std::vector<std::string> screencapArgs(const std::string& serial) {
    return {"-s", serial, "exec-out", "screencap", "-p"};
}

std::vector<std::string> logcatClearArgs(const std::string& serial) {
    return {"-s", serial, "logcat", "-c"};
}

std::vector<std::string> connectArgs(const std::string& hostPort) {
    return {"connect", hostPort};
}

std::vector<std::string> killServerArgs() {
    return {"kill-server"};
}

std::vector<std::string> parsePackageList(const std::string& output) {
    std::vector<std::string> packages;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.rfind("package:", 0) != 0) continue;
        std::string name = trim(trimmed.substr(8));
        // Some builds append " versionCode=..." or similar after the package.
        const std::size_t space = name.find(' ');
        if (space != std::string::npos) name = name.substr(0, space);
        if (!name.empty()) packages.push_back(name);
    }
    return packages;
}

}  // namespace adb::core
