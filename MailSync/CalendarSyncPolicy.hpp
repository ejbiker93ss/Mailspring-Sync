#pragma once

#include <string>

namespace CalendarSyncPolicy {
// No new timer: checked only when the existing DAV worker wakes.
inline bool reconciliationDue(long long lastSuccessfulCheck, long long now) {
    return lastSuccessfulCheck <= 0 || now < lastSuccessfulCheck ||
        now - lastSuccessfulCheck >= 60 * 60;
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
