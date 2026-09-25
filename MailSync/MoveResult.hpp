#pragma once
#include "json.hpp"
#include <string>
#include <cstdint>
#include <ctime>
#include <set>
#include <vector>
#include "SmarterMailHeaders.hpp"

namespace MoveResult {
// SmarterMail can acknowledge a move before its folder indexes expose the
// destination UID.  Keep the optimistic move stable during that bounded
// eventual-consistency window, but eventually trust a source row that never
// went away so a genuinely failed move is not hidden forever.
constexpr std::time_t pendingMoveGraceSeconds = 120;
inline bool sourceMayStillBeStale(std::time_t acceptedAt, std::time_t now) {
    return acceptedAt > 0 && now >= acceptedAt && now - acceptedAt < pendingMoveGraceSeconds;
}
inline uint32_t uid(const nlohmann::json & value) {
    try {
        const std::string s = value.is_string() ? value.get<std::string>() : value.dump();
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return 0;
        const auto n = std::stoull(s);
        return n > 0 && n < UINT32_MAX - 5 ? (uint32_t)n : 0;
    } catch (...) { return 0; }
}
inline uint32_t rowUID(const nlohmann::json & row) {
    for (const char * key : {"uid", "UID", "Uid"}) if (row.count(key)) return uid(row[key]);
    return 0;
}
// Only explicit old/new associations are accepted. Unlabelled parallel arrays
// cannot safely be matched to a batch by position.
inline uint32_t mapped(const nlohmann::json & node, uint32_t old, int depth = 0) {
    if (depth > 4) return 0;
    if (node.is_object()) {
        uint32_t source = 0;
        for (const char * key : {"uid", "UID", "sourceUID", "oldUid", "oldUID"})
            if (node.count(key)) { source = uid(node[key]); break; }
        if (source == old) for (const char * key : {"newUid", "newUID", "destinationUID", "destinationUid", "movedUID"})
            if (node.count(key) && uid(node[key])) return uid(node[key]);
        for (const char * key : {"destinationUIDs", "destinationUids", "newUIDs", "newUids", "mappedUIDs", "mappedUids", "movedUIDs", "movedUids"}) {
            if (node.count(key) && node[key].is_object() && node[key].count(std::to_string(old)))
                return uid(node[key][std::to_string(old)]);
        }
    }
    if (node.is_object() || node.is_array()) for (const auto & child : node) {
        uint32_t result = mapped(child, old, depth + 1);
        if (result) return result;
    }
    return 0;
}
inline std::string messageId(const nlohmann::json & row) {
    return SmarterMailHeaders::value(row, "message-id", {"messageId", "MessageId", "internetMessageId", "InternetMessageId", "internetMessageID", "messageID", "rfc822MessageId", "messageIdHeader"});
}
inline uint32_t newlyObserved(const std::vector<nlohmann::json> & rows, const std::set<uint32_t> & before, const std::string & mid) {
    const auto target = SmarterMailHeaders::ids(mid);
    if (target.empty()) return 0;
    uint32_t found = 0;
    for (const auto & row : rows) {
        auto id = rowUID(row);
        if (!id || before.count(id) || messageId(row) != target) continue;
        if (found && found != id) return 0; // Duplicate Message-IDs are ambiguous.
        found = id;
    }
    return found;
}
}
