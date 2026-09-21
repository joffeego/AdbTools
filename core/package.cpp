#include "core/package.h"

#include "core/strings.h"

namespace adb::core {

std::string jsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    const std::size_t start = p + needle.size();
    const std::size_t end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

std::size_t jsonStringEnd(const std::string& json, std::size_t openQuote) {
    for (std::size_t i = openQuote + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\') { ++i; continue; }
        if (c == '"') return i;
    }
    return std::string::npos;
}

std::size_t jsonObjectEnd(const std::string& json, std::size_t brace) {
    int depth = 0;
    for (std::size_t i = brace; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            const std::size_t end = jsonStringEnd(json, i);
            if (end == std::string::npos) return std::string::npos;
            i = end;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

bool findAppReleaseAsset(const std::string& json, std::string& url, std::string& row, std::string& sha) {
    url.clear();
    row.clear();
    sha.clear();

    const std::string assetsNeedle = "\"assets\":[";
    const std::size_t assets = json.find(assetsNeedle);
    if (assets == std::string::npos) return false;

    // A release can carry several .zip assets - the portable application
    // package plus the optional adb/scrcpy tools package - and they arrive in
    // upload order, so "the first .zip in the array" is not a safe way to find
    // the application archive: the updater would eventually fetch the tools
    // package and fail with "no adb_browser.exe in the update package". Prefer
    // an asset named like the application package, and fall back to the first
    // .zip so releases published before that name existed keep working.
    std::string firstUrl, firstRow, firstSha;

    std::size_t i = assets + assetsNeedle.size();
    while (i < json.size()) {
        if (json[i] != '{') {
            if (json[i] == ']') break;  // end of the asset array
            ++i;
            continue;
        }
        const std::size_t objEnd = jsonObjectEnd(json, i);
        if (objEnd == std::string::npos) break;
        const std::string entry = json.substr(i, objEnd - i + 1);
        i = objEnd + 1;

        const std::string name = jsonStringValue(entry, "name");
        const std::string assetUrl = jsonStringValue(entry, "browser_download_url");
        const bool zip = (name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0) ||
                         (assetUrl.size() >= 4 && assetUrl.compare(assetUrl.size() - 4, 4, ".zip") == 0);
        if (!zip) continue;

        const std::size_t lastSlash = assetUrl.find_last_of('/');
        const std::string assetRow =
            !name.empty() ? name : (lastSlash == std::string::npos ? assetUrl : assetUrl.substr(lastSlash + 1));

        std::string assetSha;
        const std::string digest = jsonStringValue(entry, "digest");
        if (digest.rfind("sha256:", 0) == 0) assetSha = lower(trim(digest.substr(7)));

        const std::string lowered = lower(name);
        if (lowered.find("adbfilebrowser") != std::string::npos ||
            lowered.find("adb_browser") != std::string::npos) {
            url = assetUrl;
            row = assetRow;
            sha = assetSha;
            return true;
        }

        // Remember the first .zip that is not one of the optional support
        // packages: a release carrying only tools must report "no downloadable
        // package for this version" rather than offer an update that then fails
        // to find adb_browser.exe inside the archive.
        if (firstUrl.empty() && lowered.find("tools") == std::string::npos) {
            firstUrl = assetUrl;
            firstRow = assetRow;
            firstSha = assetSha;
        }
    }

    if (!firstUrl.empty()) {
        url = firstUrl;
        row = firstRow;
        sha = firstSha;
        return true;
    }
    return false;
}

std::string findWin64AssetUrl(const std::string& json) {
    const std::size_t namePos = json.find("\"name\":\"scrcpy-win64-");
    if (namePos == std::string::npos) return "";
    const std::string needle = "\"browser_download_url\":\"";
    const std::size_t p = json.find(needle, namePos);
    if (p == std::string::npos) return "";
    const std::size_t start = p + needle.size();
    const std::size_t end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

std::string findAssetSha256(const std::string& releaseJson, const std::string& url) {
    if (url.empty()) return "";
    std::string foundUrl, foundRow, foundSha;
    (void)findAppReleaseAsset(releaseJson, foundUrl, foundRow, foundSha);
    if (foundUrl == url && foundSha.size() == 64) return foundSha;
    return "";
}

std::string xmlValue(const std::string& block, const std::string& name) {
    const std::string open = "<" + name + ">";
    const std::string close = "</" + name + ">";
    const std::size_t a = block.find(open);
    if (a == std::string::npos) return "";
    const std::size_t b = block.find(close, a);
    if (b == std::string::npos) return "";
    return trim(block.substr(a + open.size(), b - a - open.size()));
}

std::string parsePlatformToolsVersion(const std::string& xml) {
    const std::size_t pkg = xml.find("<remotePackage path=\"platform-tools\"");
    if (pkg == std::string::npos) return "";
    const std::size_t rev = xml.find("<revision>", pkg);
    if (rev == std::string::npos) return "";
    const std::size_t revEnd = xml.find("</revision>", rev);
    if (revEnd == std::string::npos) return "";
    const std::string revBlock = xml.substr(rev, revEnd - rev);
    const std::string major = xmlValue(revBlock, "major");
    if (major.empty()) return "";
    std::string v = major;
    const std::string minor = xmlValue(revBlock, "minor");
    const std::string micro = xmlValue(revBlock, "micro");
    if (!minor.empty()) v += "." + minor;
    if (!micro.empty()) v += "." + micro;
    return v;
}

void parsePlatformToolsWindowsArchive(const std::string& xml, std::string& url, std::string& checksum) {
    url.clear();
    checksum.clear();
    const std::size_t pkg = xml.find("<remotePackage path=\"platform-tools\"");
    if (pkg == std::string::npos) return;
    const std::size_t pkgEnd = xml.find("</remotePackage>", pkg);
    const std::string pkgBlock = xml.substr(pkg, pkgEnd == std::string::npos ? std::string::npos
                                                                             : pkgEnd - pkg);
    const std::size_t archives = pkgBlock.find("<archives>");
    if (archives == std::string::npos) return;

    std::size_t pos = archives;
    while ((pos = pkgBlock.find("<archive>", pos)) != std::string::npos) {
        const std::size_t end = pkgBlock.find("</archive>", pos);
        if (end == std::string::npos) return;
        const std::string block = pkgBlock.substr(pos, end - pos);
        pos = end + 1;
        if (xmlValue(block, "host-os") != "windows") continue;
        const std::string base = "https://dl.google.com/android/repository/";
        url = base + xmlValue(block, "url");
        checksum = lower(xmlValue(block, "checksum"));
        return;
    }
}

}  // namespace adb::core
