#include "SmarterMailClient.hpp"
#include "SmarterMailRawMessage.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

#include "Account.hpp"
#include "MailUtils.hpp"
#include "NetworkRequestUtils.hpp"
#include "SyncException.hpp"

using namespace nlohmann;
using namespace std;

namespace {
mutex tokenMutex;
map<string, string> tokens;
map<string, string> clientIds;

string normalizedBase(string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    if (value.find("://") == string::npos) value = "https://" + value;
    if (value.rfind("https://", 0) != 0) {
        throw SyncException("smartermail-insecure-server", "SmarterMail API connections require HTTPS.", false);
    }
    const string suffix = "/api/v1";
    if (value.size() < suffix.size() || value.substr(value.size() - suffix.size()) != suffix) {
        value += suffix;
    }
    return value;
}

json performJSON(const string & url, const string & method, const string & token, const json & payload) {
    string serialized = payload.is_null() ? "" : payload.dump();
    HTTPResponse response = PerformRequestWithStatus(CreateJSONRequest(
        url, method, token.empty() ? "" : "Bearer " + token,
        serialized.empty() ? nullptr : serialized.c_str()));
    if (response.status < 200 || response.status > 209) {
        throw SyncException("smartermail-http-" + to_string(response.status),
            url + " returned HTTP " + to_string(response.status), response.status != 401 && response.status != 403);
    }
    try { return json::parse(response.body); }
    catch (json::exception &) { return {{"text", response.body}}; }
}

string tokenFor(const string & accountId) {
    lock_guard<mutex> lock(tokenMutex);
    return tokens[accountId];
}

void saveToken(const string & accountId, const string & token) {
    lock_guard<mutex> lock(tokenMutex);
    tokens[accountId] = token;
}

string clientIdFor(const string & accountId) {
    lock_guard<mutex> lock(tokenMutex);
    auto found = clientIds.find(accountId);
    if (found != clientIds.end()) return found->second;
    string value = "WEBMAIL-" + MailUtils::idRandomlyGenerated();
    clientIds[accountId] = value;
    return value;
}

vector<json> arrayValue(const json & data, initializer_list<const char *> keys) {
    for (const char * key : keys) {
        if (data.count(key) && data[key].is_array()) {
            return data[key].get<vector<json>>();
        }
    }
    return {};
}

void flattenFolders(const json & input, const string & parent, vector<json> & output) {
    if (!input.is_array()) return;
    for (const auto & raw : input) {
        if (!raw.is_object()) continue;
        string localName = raw.value("name", raw.value("folder", raw.value("displayName", "")));
        string explicitPath = raw.value("path", "");
        string path = explicitPath.empty() ? (parent.empty() ? localName : parent + "/" + localName) : explicitPath;
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        while (!path.empty() && path.back() == '/') path.pop_back();
        if (!path.empty()) {
            json folder = raw;
            folder["path"] = path;
            folder["displayName"] = raw.value("displayName", localName);
            output.push_back(folder);
        }
        for (const char * key : {"subFolders", "subfolders", "children", "folders"}) {
            if (raw.count(key)) flattenFolders(raw[key], path.empty() ? parent : path, output);
        }
    }
}

string urlEncode(const string & value) {
    CURL * curl = curl_easy_init();
    if (!curl) return value;
    char * encoded = curl_easy_escape(curl, value.c_str(), (int)value.size());
    string result = encoded ? encoded : value;
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

string lowerCopy(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)tolower(c);
    });
    return value;
}

bool isGalSource(const json & source) {
    if (!source.is_object()) return true;
    if (source.value("isDomainResource", false)) return true;
    string id;
    for (const char * key : {"itemID", "ItemID", "sourceId", "SourceID", "id"}) {
        if (source.count(key) && source[key].is_string()) {
            id = source[key].get<string>();
            if (!id.empty()) break;
        }
    }
    return lowerCopy(id) == "gal";
}

json normalizedContactSource(json source, const string & emailAddress) {
    string username = emailAddress;
    size_t at = username.find('@');
    if (at != string::npos) username = username.substr(0, at);

    auto firstString = [&](initializer_list<const char *> keys, const string & fallback) {
        for (const char * key : keys) {
            if (source.count(key) && source[key].is_string() && !source[key].get<string>().empty()) {
                return source[key].get<string>();
            }
        }
        return fallback;
    };
    string owner = firstString(
        {"ownerUsername", "OwnerUsername", "ownerEmailAddress", "OwnerEmailAddress", "owner"},
        username);
    string name = firstString(
        {"displayName", "DisplayName", "folder", "Folder", "sourceName"}, "Contacts");
    string id = firstString(
        {"itemID", "ItemID", "sourceId", "SourceID", "folder", "Folder", "id"},
        "Contacts");
    if (!source.count("owner")) source["owner"] = owner;
    if (!source.count("id")) source["id"] = id;
    if (!source.count("sourceName")) source["sourceName"] = name;
    return source;
}
}

SmarterMailClient::SmarterMailClient(shared_ptr<Account> value) :
    account(value), baseUrl(normalizedBase(value->smarterMailServer())) {}

void SmarterMailClient::requireSuccess(const json & response, const string & operation) {
    if ((response.count("success") && response["success"].is_boolean() && !response["success"].get<bool>()) ||
        (response.count("Success") && response["Success"].is_boolean() && !response["Success"].get<bool>()) ||
        (response.count("result") && response["result"].is_boolean() && !response["result"].get<bool>()) ||
        (response.count("Result") && response["Result"].is_boolean() && !response["Result"].get<bool>())) {
        throw SyncException("smartermail-api-failure", operation + " was rejected by SmarterMail.", true);
    }
}

void SmarterMailClient::authenticate(bool force) {
    if (!force && !tokenFor(account->id()).empty()) return;
    json payload = {
        {"username", account->IMAPUsername().empty() ? account->emailAddress() : account->IMAPUsername()},
        {"password", account->IMAPPassword()},
        {"twoFactorCode", ""},
        {"clientId", clientIdFor(account->id())}
    };
    json response = performJSON(baseUrl + "/auth/authenticate-user", "POST", "", payload);
    requireSuccess(response, "Authentication");
    string accessToken = response.value("accessToken", "");
    if (accessToken.empty()) {
        if (response.value("requiresTwoFactorAuth", false)) {
            throw SyncException("smartermail-two-factor-required",
                "This SmarterMail account requires a two-factor code. Sign in to SmarterMail and use an app password for SummerMail.", false);
        }
        throw SyncException("smartermail-authentication-failed", "SmarterMail did not return an access token.", false);
    }
    saveToken(account->id(), accessToken);
}

json SmarterMailClient::requestJSON(const string & path, const string & method, const json & payload) {
    authenticate();
    try {
        json response = performJSON(baseUrl + path, method, tokenFor(account->id()), payload);
        requireSuccess(response, path);
        return response;
    } catch (SyncException & first) {
        if (first.toJSON().dump().find("smartermail-http-401") == string::npos) throw;
        // Tokens are process-local and can expire at any point. Re-authenticate once;
        // a repeated transport or API failure is allowed to propagate to the worker.
        authenticate(true);
        json response = performJSON(baseUrl + path, method, tokenFor(account->id()), payload);
        requireSuccess(response, path);
        return response;
    }
}

string SmarterMailClient::requestText(const string & path, const json & payload) {
    authenticate();
    auto run = [&]() {
        string serialized = payload.dump();
        HTTPResponse response = PerformRequestWithStatus(CreateJSONRequest(
            baseUrl + path, "POST", "Bearer " + tokenFor(account->id()), serialized.c_str()));
        if (response.status < 200 || response.status > 209) {
            throw SyncException("smartermail-http-" + to_string(response.status),
                path + " returned HTTP " + to_string(response.status), response.status != 401 && response.status != 403);
        }
        return response.body;
    };
    try { return run(); }
    catch (SyncException & first) {
        if (first.toJSON().dump().find("smartermail-http-401") == string::npos) throw;
        authenticate(true);
        return run();
    }
}

void SmarterMailClient::validate() {
    auto value = folders();
    if (value.empty()) throw SyncException("smartermail-no-folders", "SmarterMail returned no email folders.", false);
}

vector<json> SmarterMailClient::folders() {
    json response = requestJSON("/folders/list-email-folders");
    json tree = response.count("folderList") ? response["folderList"] : response.value("folders", json::array());
    vector<json> result;
    flattenFolders(tree, "", result);
    return result;
}

SmarterMailPage SmarterMailClient::messages(const string & folder, unsigned int skip, unsigned int take) {
    json response = requestJSON("/mail/messages", "POST", {
        {"ownerEmailAddress", account->emailAddress()}, {"folder", folder},
        {"skip", skip}, {"take", take}, {"selectedIds", json::array()},
        {"sortField", "internalDate"}, {"sortDirection", "desc"},
        {"sortType", "internalDate"}, {"previewLength", 200}
    });
    SmarterMailPage page;
    page.messages = arrayValue(response, {"results", "Results", "messages"});
    for (const char * key : {"totalCount", "TotalCount", "totalMessages"}) {
        if (!response.count(key) || !response[key].is_number()) continue;
        page.totalCount = response[key].get<unsigned int>();
        page.hasTotalCount = true;
        break;
    }
    return page;
}

SmarterMailMessageBody SmarterMailClient::messageBody(const string & folder, uint32_t uid) {
    json response = requestJSON("/mail/message", "POST", {
        {"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"uid", uid}
    });
    json detail = response.count("messageData") ? response["messageData"] : response;
    SmarterMailMessageBody result;

    if (detail.is_string()) {
        const string value = detail.get<string>();
        if (!SmarterMailRawMessage::isClosingBoundaryOnly(value)) {
            result.mime = SmarterMailRawMessage::normalize(value);
        }
        return result;
    }
    if (!detail.is_object()) return result;

    auto bodyValue = [&detail](initializer_list<const char *> keys) {
        for (const char * key : keys) {
            if (detail.count(key) && detail[key].is_string()) {
                const string value = detail[key].get<string>();
                // Some Outlook-generated messages expose a bogus closing MIME
                // boundary in messageHTML while a later compatibility field
                // contains the real body. Do not let the first nonempty token
                // win solely because it appeared under the preferred key.
                if (!value.empty() && !SmarterMailRawMessage::isClosingBoundaryOnly(value)) {
                    return value;
                }
            }
        }
        return string();
    };
    const string html = bodyValue({"messageHTML", "messageHtml", "htmlBody", "body"});
    const string text = bodyValue({"messagePlainText", "textBody", "plainText"});
    result.mime = SmarterMailRawMessage::structuredBodyMime(html, text);
    result.hasAttachments =
        (detail.count("attachments") && detail["attachments"].is_array() && !detail["attachments"].empty()) ||
        (detail.count("hasAttachments") && detail["hasAttachments"].is_boolean() && detail["hasAttachments"].get<bool>());
    return result;
}

string SmarterMailClient::rawMessage(const string & folder, uint32_t uid) {
    json payload = {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"uid", uid}};
    for (const string & path : {"/mail/message/raw-content", "/mail/message/raw", "/mail/message-raw-content"}) {
        try {
            string raw = requestText(path, payload);
            try {
                json envelope = json::parse(raw);
                for (const char * key : {"messageData", "data", "content", "raw", "rawContent", "message"}) {
                    if (envelope.count(key) && envelope[key].is_string() && !envelope[key].get<string>().empty()) {
                        const string value = envelope[key].get<string>();
                        if (!SmarterMailRawMessage::isClosingBoundaryOnly(value)) {
                            return SmarterMailRawMessage::normalize(value);
                        }
                    }
                }
            } catch (json::exception &) {
                if (!raw.empty() && !SmarterMailRawMessage::isClosingBoundaryOnly(raw)) {
                    return SmarterMailRawMessage::normalize(raw);
                }
            }
        } catch (SyncException &) {
            // Endpoint names vary by server version; try the next documented shape.
        }
    }
    throw SyncException("smartermail-raw-message-unavailable", "SmarterMail could not provide the raw message.", true);
}

void SmarterMailClient::markRead(const string & folder, const vector<uint32_t> & uids, bool read) {
    requestJSON("/mail/messages-patch", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"UID", uids}, {"markRead", read}});
}

void SmarterMailClient::setFlagged(const string & folder, const vector<uint32_t> & uids, bool flagged) {
    requestJSON("/mail/messages-flag-patch", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"UID", uids}, {"isFlagged", flagged}});
}

json SmarterMailClient::move(const string & folder, const vector<uint32_t> & uids, const string & destinationFolder) {
    return requestJSON("/mail/move-messages", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"UID", uids}, {"destinationFolder", destinationFolder}});
}

void SmarterMailClient::remove(const string & folder, const vector<uint32_t> & uids, bool moveToDeleted) {
    requestJSON("/mail/delete-messages", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"UID", uids}, {"moveToDeleted", moveToDeleted}});
}

static pair<string, string> splitFolderPath(const string & path) {
    size_t slash = path.find_last_of('/');
    return slash == string::npos ? make_pair(path, "")
                                 : make_pair(path.substr(slash + 1), path.substr(0, slash));
}

void SmarterMailClient::createFolder(const string & path) {
    auto parts = splitFolderPath(path);
    requestJSON("/folders/folder-put", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", parts.first}, {"parentFolder", parts.second}});
}

void SmarterMailClient::renameFolder(const string & oldPath, const string & newPath) {
    auto from = splitFolderPath(oldPath);
    auto to = splitFolderPath(newPath);
    requestJSON("/folders/folder-patch", "POST", {
        {"ownerEmailAddress", account->emailAddress()}, {"folder", from.first},
        {"parentFolder", from.second}, {"newFolder", to.first}, {"newParentFolder", to.second}
    });
}

void SmarterMailClient::deleteFolder(const string & path) {
    requestJSON("/folders/email-folder-delete", "POST", {{"ownerEmailAddress", account->emailAddress()}, {"folder", path}, {"path", path}});
}

void SmarterMailClient::importMime(const string & folder, const string & mime) {
    const vector<pair<string, json>> attempts = {
        {"/mail/save-message-mime", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"message", mime}}},
        {"/folders/email-import-mime", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"mimeContent", mime}}},
        {"/mail/message/save-mime", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"message", mime}}},
        {"/mail/import-mime", {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"mimeContent", mime}}}
    };
    for (const auto & attempt : attempts) {
        try {
            requestJSON(attempt.first, "POST", attempt.second);
            return;
        } catch (SyncException &) {}
    }
    throw SyncException("smartermail-import-mime-failed", "SmarterMail could not save the sent message.", true);
}

vector<json> SmarterMailClient::calendarSources() {
    json response = requestJSON("/calendars/sources");
    bool recognized = false;
    for (const char * key : {"calendars", "Calendars"}) {
        if (response.count(key) && response[key].is_array()) {
            recognized = true;
            if (!response[key].empty()) return response[key].get<vector<json>>();
        }
    }
    for (const char * key : {"sharedLists", "SharedLists"}) {
        if (response.count(key) && response[key].is_array()) {
            recognized = true;
            if (!response[key].empty()) return response[key].get<vector<json>>();
        }
    }
    if (!recognized) throw SyncException("smartermail-calendar-sources-invalid", "SmarterMail returned an unrecognized calendar source response.", true);
    return {};
}

vector<json> SmarterMailClient::calendarEvents(const vector<json> & sources) {
    json response = requestJSON("/calendars/events-all2", "POST", {
        {"sources", sources},
        {"searchParams", {
            {"skip", 0}, {"take", 0}, {"search", nullptr},
            {"sortField", nullptr}, {"sortDescending", false}
        }}
    });
    if (response.is_array()) return response.get<vector<json>>();
    for (const char * key : {"results", "events", "calendarEvents"}) {
        if (response.count(key) && response[key].is_array()) return response[key].get<vector<json>>();
    }
    throw SyncException("smartermail-calendar-events-invalid", "SmarterMail returned an unrecognized calendar event response.", true);
}

json SmarterMailClient::calendarEventDetails(const string & owner,
                                              const string & calendarId,
                                              const string & eventId) {
    string path = "/calendars/events/" + urlEncode(owner) + "/" +
                  urlEncode(calendarId) + "/" + urlEncode(eventId);
    try {
        json response = requestJSON(path);
        for (const char * key : {"details", "event", "result"}) {
            if (response.count(key) && response[key].is_object()) return response[key];
        }
        return response.is_object() && !response.empty() ? response : json();
    } catch (SyncException & ex) {
        if (ex.key == "smartermail-http-404") return json();
        throw;
    }
}

json SmarterMailClient::saveCalendarEvent(const string & owner,
                                           const string & calendarId,
                                           const string & eventId,
                                           const json & event) {
    string path = "/calendars/events/save/" + urlEncode(owner) + "/" + urlEncode(calendarId);
    if (!eventId.empty()) path += "/" + urlEncode(eventId);
    json response = requestJSON(path, "POST", event);
    if (response.count("events") && response["events"].is_array() && !response["events"].empty()) {
        return response["events"][0];
    }
    for (const char * key : {"event", "result"}) {
        if (response.count(key) && response[key].is_object()) return response[key];
    }
    return response;
}

void SmarterMailClient::deleteCalendarEvent(const string & owner,
                                             const string & calendarId,
                                             const string & eventId) {
    const string path = "/calendars/events/delete/" + urlEncode(owner) + "/" +
                        urlEncode(calendarId) + "/" + urlEncode(eventId) + "/false";
    json response = requestJSON(path, "POST", {{"instanceStart", nullptr}});

    bool ambiguousEmpty = response.is_array() && response.empty();
    bool allNullEcho = response.is_array() && !response.empty();
    if (allNullEcho) {
        for (const auto & item : response) {
            if (!item.is_object()) { allNullEcho = false; break; }
            bool blankOwner = !item.count("owner") || item["owner"].is_null() || item.value("owner", "").empty();
            bool blankCalendar = !item.count("calendarId") || item["calendarId"].is_null() || item.value("calendarId", "").empty();
            bool blankEvent = !item.count("eventId") || item["eventId"].is_null() || item.value("eventId", "").empty();
            if (!(blankOwner && blankCalendar && blankEvent)) { allNullEcho = false; break; }
        }
    }
    if (allNullEcho) {
        throw SyncException("smartermail-calendar-delete-noop",
            "SmarterMail accepted the calendar delete request but did not bind its identifiers.", true);
    }
    if (ambiguousEmpty && !calendarEventDetails(owner, calendarId, eventId).is_null()) {
        throw SyncException("smartermail-calendar-delete-noop",
            "SmarterMail returned an empty delete response and the event still exists.", true);
    }
}

vector<json> SmarterMailClient::contactSources() {
    vector<json> result;
    try {
        json response = requestJSON("/contacts/sources");
        auto append = [&](const json & value) {
            if (value.is_array()) {
                for (const auto & raw : value) {
                    if (raw.is_object() && !isGalSource(raw)) {
                        result.push_back(normalizedContactSource(raw, account->emailAddress()));
                    }
                }
            } else if (value.is_object() && !isGalSource(value)) {
                result.push_back(normalizedContactSource(value, account->emailAddress()));
            }
        };
        if (response.count("primaryList")) append(response["primaryList"]);
        else if (response.count("PrimaryList")) append(response["PrimaryList"]);
        if (response.count("sharedLists")) append(response["sharedLists"]);
        else if (response.count("SharedLists")) append(response["SharedLists"]);
    } catch (SyncException &) {
        // The user's primary Contacts folder exists even on builds whose
        // sources endpoint is malformed or unavailable.
    }
    if (result.empty()) {
        string username = account->emailAddress();
        size_t at = username.find('@');
        if (at != string::npos) username = username.substr(0, at);
        result.push_back({
            {"ownerEmailAddress", account->emailAddress()},
            {"ownerUsername", account->emailAddress()}, {"folder", "Contacts"},
            {"owner", username}, {"id", "Contacts"}, {"sourceName", "Contacts"}
        });
    }
    return result;
}

SmarterMailContactPage SmarterMailClient::contacts(const vector<json> & sources,
                                                    unsigned int skip,
                                                    unsigned int take,
                                                    const string & query) {
    json response = requestJSON("/contacts/contacts-all", "POST", {
        {"sources", sources},
        {"searchParams", {
            {"categories", nullptr}, {"showNonCategorized", true},
            {"skip", skip}, {"take", take},
            {"search", query.empty() ? json(nullptr) : json(query)},
            {"sortDescending", false}
        }}
    });
    SmarterMailContactPage page;
    bool recognized = response.is_array();
    if (response.is_array()) page.contacts = response.get<vector<json>>();
    else for (const char * key : {"contactList", "results", "contacts"}) {
        if (response.count(key) && response[key].is_array()) {
            page.contacts = response[key].get<vector<json>>();
            recognized = true;
            break;
        }
    }
    if (!recognized) throw SyncException("smartermail-contacts-invalid", "SmarterMail returned an unrecognized contact list response.", true);
    for (const char * key : {"totalCount", "TotalCount"}) {
        if (response.count(key) && response[key].is_number()) {
            page.totalCount = response[key].get<unsigned int>();
            page.hasTotalCount = true;
            break;
        }
    }
    return page;
}

json SmarterMailClient::saveContact(const string & contactId, const json & contact) {
    string path = "/contacts/contact-put/" + urlEncode(account->emailAddress());
    if (!contactId.empty()) path += "/" + urlEncode(contactId);
    return requestJSON(path, "POST", contact);
}

void SmarterMailClient::deleteContact(const string & sourceOwner,
                                      const string & sourceId,
                                      const string & contactId) {
    requestJSON("/contacts/delete-bulk", "POST", json::array({{
        {"sourceOwner", sourceOwner}, {"sourceId", sourceId}, {"id", contactId}
    }}));
}
