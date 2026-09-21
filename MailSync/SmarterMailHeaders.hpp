#pragma once
#include "json.hpp"
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace SmarterMailHeaders {
inline std::string decoded(std::string input) {
    // SmarterMail JSON may HTML-encode the RFC header, not just its body.
    for (const auto & pair : {std::make_pair("&lt;", "<"), std::make_pair("&gt;", ">"),
                             std::make_pair("&#60;", "<"), std::make_pair("&#62;", ">"),
                             std::make_pair("&amp;", "&")}) {
        size_t at = 0;
        while ((at = input.find(pair.first, at)) != std::string::npos) {
            input.replace(at, std::char_traits<char>::length(pair.first), pair.second);
            at += std::char_traits<char>::length(pair.second);
        }
    }
    return input;
}
// Match the case-preserving RFC identity extraction used by the reference client.
inline std::string ids(const nlohmann::json & value) {
    std::string input;
    if (value.is_string()) input = value.get<std::string>();
    else if (value.is_array()) for (const auto & v : value) if (v.is_string()) input += v.get<std::string>() + " ";
    input = decoded(input);
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
    // Native message detail exposes the original RFC header block in `header`.
    // Extract only identity fields; never prepend its MIME envelope to the
    // separately reconstructed HTML/plain body. References may be folded.
    if (data.count("header") && data["header"].is_string()) {
        std::istringstream lines(data["header"].get<std::string>());
        std::string line, field, contents;
        auto flush = [&]() { if (field == name) result += ids(contents); };
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line[0] == ' ' || line[0] == '\t') { contents += " " + line; continue; }
            flush();
            const auto colon = line.find(':');
            field = colon == std::string::npos ? "" : line.substr(0, colon);
            contents = colon == std::string::npos ? "" : line.substr(colon + 1);
            for (char & c : field) c = (char)std::tolower((unsigned char)c);
        }
        flush();
    }
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
