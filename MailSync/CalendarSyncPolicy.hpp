#pragma once

#include <string>

namespace CalendarSyncPolicy {
// No new timer: checked only when the existing DAV worker wakes.
inline bool reconciliationDue(long long lastSuccessfulCheck, long long now) {
    return lastSuccessfulCheck <= 0 || now < lastSuccessfulCheck ||
        now - lastSuccessfulCheck >= 60 * 60;
}

// Keep resource downloads on the authenticated collection origin. Preserve URL
// escaping and literal '+'; these are paths, not form-encoded query strings.
inline std::string resourcePath(const std::string& href) {
    auto scheme = href.find("://");
    if (scheme == std::string::npos) return href;
    auto path = href.find('/', scheme + 3);
    return path == std::string::npos ? "" : href.substr(path);
}

// Escape XML text without changing the URL's percent encoding or literal '+'.
inline std::string hrefText(const std::string& value) {
    std::string result;
    for (char c : value) {
        if (c == '&') result += "&amp;";
        else if (c == '<') result += "&lt;";
        else if (c == '>') result += "&gt;";
        else result += c;
    }
    return result;
}
}
