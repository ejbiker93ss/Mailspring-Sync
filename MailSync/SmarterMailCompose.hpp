#pragma once
#include "Message.hpp"
#include "MailStore.hpp"
#include "SmarterMailHeaders.hpp"
#include "SyncException.hpp"
#include <functional>
#include <random>
#include <set>

namespace SmarterMailCompose {
inline std::string attachmentNameKey(std::string filename) {
    for (char & ch : filename) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    return filename;
}

inline std::string uniqueAttachmentName(const std::string & filename,
                                        const std::set<std::string> & originalNames,
                                        std::set<std::string> & stagedNames) {
    if (stagedNames.insert(attachmentNameKey(filename)).second) return filename;

    const auto dot = filename.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && dot > 0 && dot + 1 < filename.size();
    const auto stem = hasExtension ? filename.substr(0, dot) : filename;
    const auto extension = hasExtension ? filename.substr(dot) : "";
    for (size_t number = 2; ; ++number) {
        const auto candidate = stem + " (" + std::to_string(number) + ")" + extension;
        const auto key = attachmentNameKey(candidate);
        if (!originalNames.count(key) && stagedNames.insert(key).second) return candidate;
    }
}

inline bool usableParent(Message & draft, Message & parent) {
    return parent.accountId() == draft.accountId() && parent.remoteUID() != 0 &&
        parent.remoteUID() <= UINT32_MAX - 5 &&
        !parent.remoteFolder().value("path", "").empty();
}

// A moved message can leave a nonphysical local placeholder at the ID saved
// in a draft. Recover only from a unique physical copy of the same Internet ID
// in the same account; guessing between duplicates could forward the wrong mail.
using MovedMessageLookup = std::function<uint32_t(const std::string &, const std::string &, const std::string &)>;
inline std::shared_ptr<Message> resolveParent(MailStore & store, Message & draft,
                                              const MovedMessageLookup & lookup = {}) {
    std::shared_ptr<Message> parent;
    const auto localParent = draft._data.find("replyToMessageId");
    if (localParent != draft._data.end() && localParent->is_string()) {
        parent = store.find<Message>(Query().equal("accountId", draft.accountId())
            .equal("id", localParent->get<std::string>()));
    }
    if (parent && usableParent(draft, *parent)) return parent;

    const std::string mid = draft.forwardedHeaderMessageId().empty()
        ? draft.replyToHeaderMessageId() : draft.forwardedHeaderMessageId();
    std::string destination, moveId;
    if (parent && parent->_data.count("smPendingMove") && parent->_data["smPendingMove"].is_object()) {
        const auto & move = parent->_data["smPendingMove"];
        destination = move.value("destinationFolder", "");
        moveId = SmarterMailHeaders::ids(move.value("messageId", ""));
        if (destination.empty() || destination != parent->remoteFolder().value("path", "")) return nullptr;
    }
    std::shared_ptr<Message> match;
    if (!mid.empty()) {
        for (auto & candidate : store.findAll<Message>(Query().equal("accountId", draft.accountId())
            .equal("headerMessageId", mid))) {
            if (!usableParent(draft, *candidate)) continue;
            if (!destination.empty() && candidate->remoteFolder().value("path", "") != destination) continue;
            if (match && match->id() != candidate->id()) return nullptr;
            match = candidate;
        }
    }
    if (match) return match;
    if (!lookup || moveId.empty() || (!mid.empty() && SmarterMailHeaders::ids(mid) != moveId)) return nullptr;
    const auto uid = lookup(destination, parent->subject(), moveId);
    if (!uid || uid > UINT32_MAX - 5) return nullptr;
    // Only the send needs these coordinates. The sync worker owns cache reconciliation.
    parent->setRemoteUID(uid);
    return parent;
}

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
        if (!parent || !usableParent(draft, *parent)) {
            // The forward draft already contains the quoted message and its files.
            // Only the server-side link to the original requires a physical parent.
            if (forward) return result;
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
