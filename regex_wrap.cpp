// regex_wrap.cpp — a thin exception-isolated wrapper around std::regex.
//
// The rest of the app is compiled with -fno-exceptions (the EUI-NEO app
// template), so std::regex's invalid-pattern throw would terminate the process.
// This single translation unit is compiled WITH exceptions so a bad user regex
// degrades to "invalid" instead of crashing.
#include <regex>
#include <string>

extern "C" void* adbRegexCompile(const char* pattern, int ignoreCase) {
    if (pattern == nullptr || *pattern == '\0') {
        return nullptr;
    }
    try {
        std::regex::flag_type flags = std::regex::ECMAScript | std::regex::optimize;
        if (ignoreCase) {
            flags |= std::regex::icase;
        }
        return reinterpret_cast<void*>(new std::regex(pattern, flags));
    } catch (...) {
        return nullptr;
    }
}

extern "C" int adbRegexSearchHandle(void* handle, const char* text) {
    if (handle == nullptr || text == nullptr) {
        return 0;
    }
    try {
        const std::regex* re = reinterpret_cast<const std::regex*>(handle);
        return std::regex_search(std::string(text), *re) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" void adbRegexFree(void* handle) {
    delete reinterpret_cast<std::regex*>(handle);
}
