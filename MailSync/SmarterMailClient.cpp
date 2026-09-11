#include "SmarterMailClient.hpp"

#include <algorithm>
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

string SmarterMailClient::rawMessage(const string & folder, uint32_t uid) {
    json payload = {{"ownerEmailAddress", account->emailAddress()}, {"folder", folder}, {"uid", uid}};
    for (const string & path : {"/mail/message/raw-content", "/mail/message/raw", "/mail/message-raw-content"}) {
        try {
            string raw = requestText(path, payload);
            try {
                json envelope = json::parse(raw);
                for (const char * key : {"messageData", "data", "content", "raw", "rawContent", "message"}) {
                    if (envelope.count(key) && envelope[key].is_string() && !envelope[key].get<string>().empty()) {
                        return envelope[key].get<string>();
                    }
                }
            } catch (json::exception &) {
                if (!raw.empty()) return raw;
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
