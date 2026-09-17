#ifndef SmarterMailRawMessage_hpp
#define SmarterMailRawMessage_hpp

#include <cctype>
#include <string>

namespace SmarterMailRawMessage {

inline size_t firstContentOffset(const std::string & value) {
    size_t offset = 0;

    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        offset = 3;
    }

    while (offset < value.size() &&
           std::isspace(static_cast<unsigned char>(value[offset]))) {
        offset++;
    }
    return offset;
}

inline bool startsWithCaseInsensitive(
    const std::string & value,
    size_t offset,
    const char * prefix
) {
    for (size_t ii = 0; prefix[ii] != '\0'; ii++) {
        if (offset + ii >= value.size()) return false;
        const auto actual = static_cast<unsigned char>(value[offset + ii]);
        const auto expected = static_cast<unsigned char>(prefix[ii]);
        if (std::tolower(actual) != std::tolower(expected)) return false;
    }
    return true;
}

inline bool isBareHTMLDocument(const std::string & value) {
    const size_t offset = firstContentOffset(value);
    return startsWithCaseInsensitive(value, offset, "<!doctype html") ||
           startsWithCaseInsensitive(value, offset, "<html") ||
           startsWithCaseInsensitive(value, offset, "<head") ||
           startsWithCaseInsensitive(value, offset, "<body");
}

inline std::string normalize(const std::string & value) {
    if (!isBareHTMLDocument(value)) return value;

    return "MIME-Version: 1.0\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Content-Transfer-Encoding: 8bit\r\n"
           "\r\n" + value;
}

} // namespace SmarterMailRawMessage

#endif
