#pragma once
#include "json.hpp"
#include <regex>
#include <set>
#include <string>

namespace SmarterMailHeaders {
// Match the case-preserving RFC identity extraction used by the reference client.
inline std::string ids(const nlohmann::json & value) {
    std::string input;
    if (value.is_string()) input = value.get<std::string>();
    else if (value.is_array()) for (const auto & v : value) if (v.is_string()) input += v.get<std::string>() + " ";
    static const std::regex pattern("<([^<>\\s]+)>|([^<>\\s]+@[^<>\\s]+)");
    std::string result;
    std::set<std::string> seen;
    for (std::sregex_iterator it(input.begin(), input.end(), pattern), end; it != end; ++it) {
        const std::string id = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        if (seen.insert(id).second) result += "<" + id + "> ";
        if (seen.size() >= 50) break;
    }
    return result;
}
inline std::string value(const nlohmann::json & data, const std::string & name,
                         std::initializer_list<const char *> aliases) {
    std::string result;
    for (const char * key : aliases) if (data.count(key)) result += ids(data[key]);
    for (const char * key : {"headers", "customHeaders", "additionalHeaders"}) {
        if (!data.count(key) || !data[key].is_array()) continue;
        for (const auto & h : data[key]) {
            if (!h.is_object() || !h.count("name") || !h["name"].is_string()) continue;
            std::string n = h["name"].get<std::string>();
            for (char & c : n) c = (char)std::tolower((unsigned char)c);
            if (n == name && h.count("value")) result += ids(h["value"]);
        }
    }
    return ids(result);
}
inline std::string mime(const nlohmann::json & data) {
    const auto mid = value(data, "message-id", {"internetMessageId", "internetMessageID", "rfc822MessageId", "messageID", "messageIdHeader"});
    const auto refs = value(data, "references", {"references", "referencesHeader"});
    const auto reply = value(data, "in-reply-to", {"inReplyTo", "in_reply_to", "inReplyToHeader", "inReplyToMessageId"});
    std::string out;
    if (!mid.empty()) out += "Message-ID: " + mid.substr(0, mid.find('>') + 1) + "\r\n";
    if (!refs.empty() || !reply.empty()) out += "References: " + ids(refs + reply) + "\r\n";
    if (!reply.empty()) out += "In-Reply-To: " + reply + "\r\n";
    return out;
}
}
