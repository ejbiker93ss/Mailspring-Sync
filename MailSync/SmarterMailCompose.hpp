#pragma once
#include "Message.hpp"
#include "SyncException.hpp"
#include <random>

namespace SmarterMailCompose {
inline std::string guid() {
    std::random_device random;
    const char * hex = "0123456789abcdef";
    std::string value;
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) value += '-';
        const unsigned int digit = random() & 15;
        value += hex[i == 12 ? 4 : (i == 16 ? ((digit & 3) | 8) : digit)];
    }
    return value;
}
inline std::string addresses(const nlohmann::json & contacts) {
    std::string out;
    for (const auto & contact : contacts) {
        if (!contact.is_object() || !contact.count("email") || !contact["email"].is_string()) continue;
        if (!out.empty()) out += ", ";
        out += contact["email"].get<std::string>();
    }
    return out;
}
// Native reply identity is the physical parent, not a cached Internet Message-ID.
// Mirrors smSendMessage's message-put payload in Smarter-Mail.
inline nlohmann::json payload(Message & draft, const std::string & owner,
                              const std::string & body, bool plaintext, Message * parent) {
    nlohmann::json result = {{"to", addresses(draft.to())}, {"cc", addresses(draft.cc())},
        {"bcc", addresses(draft.bcc())}, {"subject", draft.subject()}, {"from", ""},
        {"priority", "Normal"}, {"messageHTML", plaintext ? "" : body},
        {"messagePlainText", plaintext ? body : ""}};
    const auto importance = draft._data.count("hImportance") && draft._data["hImportance"].is_string()
        ? draft._data["hImportance"].get<std::string>() : "normal";
    if (importance == "high") result["priority"] = "High";
    if (importance == "low") result["priority"] = "Low";
    const bool forward = !draft.forwardedHeaderMessageId().empty();
    const bool reply = !forward && (!draft.replyToHeaderMessageId().empty() ||
        (draft._data.count("replyToMessageId") && draft._data["replyToMessageId"].is_string() &&
         !draft._data["replyToMessageId"].get<std::string>().empty()));
    if (reply || forward) {
        if (!parent || parent->accountId() != draft.accountId() || !parent->remoteUID() ||
            parent->remoteUID() > UINT32_MAX - 5 || parent->remoteFolder().value("path", "").empty()) {
            throw SyncException("smartermail-reply-parent-missing", "Cannot locate the original message on SmarterMail. Reopen it and reply again; no email was sent.", false);
        }
        result["replyUid"] = parent->remoteUID();
        result["replyFromFolder"] = parent->remoteFolder()["path"];
        result["replyOwner"] = owner;
        result[forward ? "isForward" : "isReply"] = true;
        result["actions"] = {{forward ? "Forwarded" : "Replied", true}};
    }
    // No Message-ID, In-Reply-To, or References overrides: the server derives them.
    return result;
}
}
