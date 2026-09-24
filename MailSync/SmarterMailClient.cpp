#include "SmarterMailClient.hpp"
#include "SmarterMailHeaders.hpp"
#include "SmarterMailRawMessage.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <regex>

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

json performJSON(const string & url, const string & method, const string & token, const json & payload, long timeout = 0) {
    string serialized = payload.is_null() ? "" : payload.dump();
    CURL * request = CreateJSONRequest(
        url, method, token.empty() ? "" : "Bearer " + token,
        serialized.empty() ? nullptr : serialized.c_str());
    if (timeout) curl_easy_setopt(request, CURLOPT_TIMEOUT, timeout);
    HTTPResponse response = PerformRequestWithStatus(request);
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

string replaceAmpEntities(string value) {
    size_t at = 0;
    while ((at = value.find("&amp;", at)) != string::npos) value.replace(at, 5, "&");
    return value;
}

bool isSafeInlineAttachmentUrl(const string & value) {
    if (value.size() > 8192 || value.find('\\') != string::npos || value.find('\0') != string::npos) return false;
    const string prefix = "/attachment/";
    if (value.rfind(prefix, 0) != 0) return false;
    const size_t query = value.find('?');
    if (query == string::npos || query == prefix.size() || value.find("..") != string::npos) return false;
    const string operation = lowerCopy(value.substr(prefix.size(), query - prefix.size()));
    return operation == "inline" || operation == "preview" || operation == "download";
}

string imageContentType(const string & bytes, const string & url) {
    if (bytes.size() >= 8 && (unsigned char)bytes[0] == 0x89 && bytes.substr(1, 3) == "PNG") return "image/png";
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xd8 && (unsigned char)bytes[2] == 0xff) return "image/jpeg";
    if (bytes.size() >= 6 && (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a")) return "image/gif";
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") return "image/webp";
    const string lower = lowerCopy(url);
    if (lower.find(".png") != string::npos) return "image/png";
    if (lower.find(".jpg") != string::npos || lower.find(".jpeg") != string::npos) return "image/jpeg";
    if (lower.find(".gif") != string::npos) return "image/gif";
    if (lower.find(".webp") != string::npos) return "image/webp";
    return "";
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
    json response = performJSON(baseUrl + "/auth/authenticate-user", "POST", "", payload, requestTimeout);
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
        json response = performJSON(baseUrl + path, method, tokenFor(account->id()), payload, requestTimeout);
        requireSuccess(response, path);
        return response;
    } catch (SyncException & first) {
        if (first.toJSON().dump().find("smartermail-http-401") == string::npos) throw;
        // Tokens are process-local and can expire at any point. Re-authenticate once;
        // a repeated transport or API failure is allowed to propagate to the worker.
        authenticate(true);
        json response = performJSON(baseUrl + path, method, tokenFor(account->id()), payload, requestTimeout);
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

string SmarterMailClient::inlineAttachmentBytes(const string & relativeUrl) {
    const string decodedUrl = replaceAmpEntities(relativeUrl);
    if (!isSafeInlineAttachmentUrl(decodedUrl)) {
        throw SyncException("smartermail-inline-attachment-url", "SmarterMail returned an unsafe inline attachment URL.", true);
    }
    const string suffix = "/api/v1";
    if (baseUrl.size() <= suffix.size() || baseUrl.substr(baseUrl.size() - suffix.size()) != suffix) {
        throw SyncException("smartermail-inline-attachment-url", "SmarterMail server URL is invalid.", true);
    }
    const string url = baseUrl.substr(0, baseUrl.size() - suffix.size()) + decodedUrl;
    authenticate();
    auto run = [&]() {
        CURL * request = CreateJSONRequest(url, "GET", "Bearer " + tokenFor(account->id()));
        curl_easy_setopt(request, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(request, CURLOPT_MAXFILESIZE, 10L * 1024L * 1024L);
        HTTPResponse response = PerformRequestWithStatus(request);
        if (response.status < 200 || response.status > 209) {
            throw SyncException("smartermail-http-" + to_string(response.status),
                "SmarterMail inline attachment returned HTTP " + to_string(response.status), response.status != 401 && response.status != 403);
        }
        if (response.body.empty() || response.body.size() > 10 * 1024 * 1024) {
            throw SyncException("smartermail-inline-attachment-size", "SmarterMail inline attachment is empty or too large.", true);
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

string SmarterMailClient::materializeInlineImageUrls(const string & html) {
    // SmarterMail returns pasted images as authenticated, server-relative
    // /attachment/inline URLs. They cannot be handed to Electron directly:
    // that would both fail to load and expose the credential path. Fetch only
    // the known attachment routes here and persist the safe image data URL.
    static const regex sourceAttribute(R"((\bsrc\s*=\s*["'])(/attachment/(?:inline|preview|download)\?[^"']+)(["']))", regex::icase);
    string result;
    size_t copied = 0;
    size_t imageCount = 0;
    map<string, string> replacements;
    for (sregex_iterator it(html.begin(), html.end(), sourceAttribute), end; it != end; ++it) {
        const smatch & match = *it;
        const string originalUrl = match.str(2);
        string replacement;
        auto cached = replacements.find(originalUrl);
        if (cached != replacements.end()) {
            replacement = cached->second;
        } else if (imageCount < 12) {
            ++imageCount;
            try {
                const string bytes = inlineAttachmentBytes(originalUrl);
                const string type = imageContentType(bytes, originalUrl);
                if (!type.empty()) replacement = "data:" + type + ";base64," + MailUtils::toBase64(bytes.data(), bytes.size());
            } catch (const SyncException &) {
                // Keep the original source on a transient fetch failure. The
                // next normal hydration pass can retry without losing mail.
            }
            replacements[originalUrl] = replacement;
        }
        if (replacement.empty()) continue;
        result.append(html, copied, (size_t)match.position() - copied);
        result += match.str(1) + replacement + match.str(3);
        copied = (size_t)match.position() + match.length();
    }
    if (copied == 0) return html;
    result.append(html, copied, string::npos);
    return result;
}

void SmarterMailClient::sendMessage(const json & payload) {
    // No SMTP fallback/retry after an ambiguous submission: it could duplicate delivery.
    requestJSON("/mail/message-put", "POST", payload);
}

void SmarterMailClient::uploadComposeAttachment(const string & guid, const string & filename,
                                               const string & contentType, const string & bytes) {
    authenticate();
    CURL * request = CreateJSONRequest(baseUrl + "/mail/attachment/" + guid, "POST", "Bearer " + tokenFor(account->id()));
    curl_mime * form = curl_mime_init(request);
    curl_mimepart * part = curl_mime_addpart(form);
    curl_mime_name(part, "file");
    curl_mime_filename(part, filename.c_str());
    curl_mime_type(part, contentType.c_str());
    curl_mime_data(part, bytes.data(), bytes.size());
    curl_easy_setopt(request, CURLOPT_MIMEPOST, form);
    HTTPResponse response;
    try { response = PerformRequestWithStatus(request); }
    catch (...) { curl_mime_free(form); throw; }
    curl_mime_free(form);
    if (response.status < 200 || response.status >= 300)
        throw SyncException("smartermail-attachment-upload", "SmarterMail rejected the attachment upload; no message was sent.", false);
    if (!response.body.empty()) {
        try { requireSuccess(json::parse(response.body), "Attachment upload"); }
        catch (const json::exception &) {} // Some supported builds return an empty/plain success response.
    }
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

    // Keep enough non-content metadata to diagnose version-specific response
    // shapes without ever writing mail text, headers, addresses, or tokens to
    // the log. This is emitted only when every body candidate is invalid.
    size_t described = 0;
    for (const auto & entry : detail.items()) {
        if (described++ >= 80 || entry.key().size() > 64) break;
        if (!result.responseShape.empty()) result.responseShape += ", ";
        result.responseShape += entry.key() + ":" + entry.value().type_name();
        if (entry.value().is_string()) {
            const string value = entry.value().get<string>();
            result.responseShape += "[" + to_string(value.size());
            if (SmarterMailRawMessage::isClosingBoundaryOnly(value)) result.responseShape += ",closing-boundary";
            result.responseShape += "]";
        } else if (entry.value().is_array() || entry.value().is_object()) {
            result.responseShape += "[" + to_string(entry.value().size()) + "]";
        }
    }

    for (const char * key : {"replyUid", "replyUID", "replyToUid", "inReplyToUid"}) {
        if (!detail.count(key)) continue;
        try {
            const auto uid = detail[key].is_string() ? stoull(detail[key].get<string>()) : detail[key].get<uint64_t>();
            if (uid > 0 && uid < UINT32_MAX - 5) { result.replyUid = (uint32_t)uid; break; }
        } catch (...) {}
    }
    for (const char * key : {"replyFromFolder", "replyFolder", "replyToFolder"})
        if (detail.count(key) && detail[key].is_string() && !detail[key].get<string>().empty()) { result.replyFolder = detail[key].get<string>(); break; }
    if (detail.count("replyOwner") && detail["replyOwner"].is_string()) result.replyOwner = detail["replyOwner"].get<string>();

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
    // The structured Mail Message response is the authoritative display
    // payload.  In particular, some SmarterMail versions put Outlook's
    // complete RFC822/MIME source in `raw` while messageHTML contains only
    // the final multipart delimiter.  The separate raw-content endpoint can
    // return that same delimiter, so do not use it as the primary recovery
    // path for a message body.
    auto bodyPartValue = [&detail](bool htmlPart) {
        for (const char * partsKey : {"bodyParts", "parts", "messageParts", "alternativeParts"}) {
            if (!detail.count(partsKey) || !detail[partsKey].is_array()) continue;
            for (const auto & part : detail[partsKey]) {
                if (!part.is_object()) continue;
                string contentType;
                for (const char * typeKey : {"contentType", "mimeType", "type"}) {
                    if (part.count(typeKey) && part[typeKey].is_string()) {
                        contentType = part[typeKey].get<string>();
                        transform(contentType.begin(), contentType.end(), contentType.begin(),
                            [](unsigned char c) { return (char)tolower(c); });
                        break;
                    }
                }
                const bool matchingType = htmlPart
                    ? contentType.rfind("text/html", 0) == 0
                    : contentType.rfind("text/plain", 0) == 0;
                if (!matchingType) continue;
                for (const char * valueKey : {"content", "body", "text", "value", "data"}) {
                    if (!part.count(valueKey) || !part[valueKey].is_string()) continue;
                    const string value = part[valueKey].get<string>();
                    if (!value.empty() && !SmarterMailRawMessage::isClosingBoundaryOnly(value)) return value;
                }
            }
        }
        return string();
    };
    string html = bodyValue({"messageHTML", "messageHtml", "htmlBody", "body"});
    string text = bodyValue({"messagePlainText", "textBody", "plainText"});
    if (html.empty()) html = bodyPartValue(true);
    if (text.empty()) text = bodyPartValue(false);
    result.mime = SmarterMailRawMessage::structuredBodyMime(html, text);
    if (!result.mime.empty()) result.mime = SmarterMailHeaders::mime(detail) + result.mime;
    if (result.mime.empty()) {
        // `raw` is part of the successful /mail/message response, not the
        // unreliable raw-content compatibility endpoint.  It can be a full
        // RFC822 document or a headerless multipart payload; normalize()
        // handles both forms before MailCore parses the body.
        const string raw = bodyValue({"raw", "rawContent", "messageRaw", "mimeContent"});
        if (!raw.empty()) result.mime = SmarterMailRawMessage::normalize(raw);
    }
    // The detail and list APIs do not use a perfectly consistent shape across
    // SmarterMail versions. Treat either the explicit flag or a non-empty
    // attachment collection as authoritative. This is deliberately only a
    // hint: the sync worker uses it to decide whether the heavier RFC822 fetch
    // is necessary to preserve MIME parts and Content-IDs.
    auto attachmentFlag = [&detail](const char * key) {
        if (!detail.count(key) || detail[key].is_null()) return false;
        const auto & value = detail[key];
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_number()) return value.get<double>() != 0;
        if (value.is_string()) {
            string text = value.get<string>();
            transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)tolower(c); });
            return text == "true" || text == "1" || text == "yes";
        }
        return false;
    };
    result.hasAttachments = attachmentFlag("hasAttachments") || attachmentFlag("hasAttachment");
    for (const char * key : {"attachments", "attachmentList", "messageAttachments"}) {
        if (detail.count(key) && detail[key].is_array() && !detail[key].empty()) {
            result.hasAttachments = true;
            result.attachments = detail[key].get<vector<json>>();
            break;
        }
    }
    return result;
}

string SmarterMailClient::attachmentBytes(const json & attachment, const string & folder,
                                          uint32_t uid, size_t index) {
    authenticate();
    const string suffix = "/api/v1";
    if (baseUrl.size() <= suffix.size() || baseUrl.substr(baseUrl.size() - suffix.size()) != suffix) {
        throw SyncException("smartermail-attachment-url", "SmarterMail server URL is invalid.", true);
    }
    const string serverRoot = baseUrl.substr(0, baseUrl.size() - suffix.size());
    vector<string> candidates;
    for (const char * key : {"downloadUrl", "url", "link", "previewImage"}) {
        if (!attachment.count(key) || !attachment[key].is_string()) continue;
        string value = replaceAmpEntities(attachment[key].get<string>());
        if (value.rfind("~/", 0) == 0) value = value.substr(1);
        // Bearer credentials must never be sent to an attachment-supplied
        // absolute host. SmarterMail's supported links are server-relative.
        if (isSafeInlineAttachmentUrl(value)) candidates.push_back(serverRoot + value);
    }
    auto encoded = [](const string & value) {
        CURL * curl = curl_easy_init();
        char * escaped = curl_easy_escape(curl, value.c_str(), (int)value.size());
        string result = escaped ? escaped : "";
        if (escaped) curl_free(escaped);
        curl_easy_cleanup(curl);
        return result;
    };
    string filename = "attachment";
    for (const char * key : {"filename", "fileName", "name"}) {
        if (attachment.count(key) && attachment[key].is_string() && !attachment[key].get<string>().empty()) {
            filename = attachment[key].get<string>();
            break;
        }
    }
    size_t part = index;
    for (const char * key : {"partID", "partId", "index"}) {
        if (!attachment.count(key)) continue;
        try {
            part = attachment[key].is_string()
                ? (size_t)stoull(attachment[key].get<string>())
                : (size_t)attachment[key].get<uint64_t>();
            break;
        } catch (...) {}
    }
    candidates.push_back(baseUrl + "/mail/attachment/" + encoded(folder) + "/" +
        to_string(uid) + "/" + to_string(part) + "/" + encoded(filename));
    candidates.push_back(baseUrl + "/mail/attachment/" + encoded(folder) + "/" +
        to_string(uid) + "/" + to_string(part));
    candidates.push_back(baseUrl + "/mail/get-attachment?folder=" + encoded(folder) +
        "&uid=" + to_string(uid) + "&index=" + to_string(part) +
        "&ownerEmailAddress=" + encoded(account->emailAddress()));

    auto run = [&]() -> string {
        for (const auto & url : candidates) {
            CURL * request = CreateJSONRequest(url, "GET", "Bearer " + tokenFor(account->id()));
            curl_easy_setopt(request, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(request, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(50LL * 1024 * 1024));
            HTTPResponse response = PerformRequestWithStatus(request);
            if (response.status >= 200 && response.status <= 209 && !response.body.empty() &&
                response.body.size() <= 50ULL * 1024 * 1024) return response.body;
        }
        return {};
    };
    string bytes = run();
    if (!bytes.empty()) return bytes;
    authenticate(true);
    bytes = run();
    if (!bytes.empty()) return bytes;
    throw SyncException("smartermail-attachment-unavailable",
        "SmarterMail could not provide the attachment.", true);
}

nlohmann::json SmarterMailClient::taskOperation(const json & request) {
    requestTimeout = 15;
    const string operation = request.at("operation").get<string>();
    const auto sources = requestJSON("/tasks/sources").at("sources");
    if (operation == "sources") return sources;
    const string sourceId = request.at("sourceId").get<string>();
    const string owner = request.at("owner").get<string>();
    json source;
    for (const auto & candidate : sources)
        if (candidate.value("id", string()) == sourceId && candidate.value("owner", string()) == owner) source = candidate;
    if (source.is_null()) throw SyncException("task-source-unavailable", "Task list is no longer available. Refresh the lists.", false);
    if (operation == "list") {
        return requestJSON("/tasks/tasks-all", "POST", {{"sources", json::array({{{"owner", owner}, {"id", sourceId}}})},
            {"searchParams", {{"skip", max(0, request.value("skip", 0))}, {"take", 100}, {"search", ""},
                {"sortField", "subject"}, {"sortDescending", false}, {"categories", json::array()},
                {"showNonCategorized", true}, {"filterFlags", json::object()}}}});
    }
    const string id = request.value("taskId", string());
    auto segment = [](const string & value) { CURL * c = curl_easy_init(); char * p = curl_easy_escape(c, value.c_str(), (int)value.size()); string s(p); curl_free(p); curl_easy_cleanup(c); return s; };
    const string detailPath = "/tasks/" + segment(owner) + "/" + segment(sourceId) + "/" + segment(id);
    if (operation == "detail") return requestJSON(detailPath);
    // Shared task lists remain readable. Do not infer write rights from visibility.
    if (source.value("isSharedItem", false) || owner != account->emailAddress())
        throw SyncException("task-list-read-only", "Edit shared tasks in SmarterMail webmail.", false);
    if (operation != "save" && operation != "delete") throw SyncException("invalid-task-operation", "Unsupported task operation.", false);
    json task = {{"sourceOwner", owner}, {"sourceId", sourceId}, {"subject", ""}, {"description", ""},
        {"status", 0}, {"percentComplete", 0}, {"priority", 5}, {"useDateTime", false}, {"reminderSet", false}};
    if (!id.empty()) {
        const auto details = requestJSON(detailPath).at("details");
        if (details.empty()) throw SyncException("task-not-found", "Task no longer exists. Refresh the list.", false);
        task = details.at(0); // Preserve recurrence, attachments and other unedited fields.
        if (task.value("id", string()) != id || task.value("sourceId", string()) != sourceId || task.value("sourceOwner", string()) != owner)
            throw SyncException("task-identity-mismatch", "Task identity changed. Refresh the list.", false);
        if (task.value("isDelegatedByOwner", false))
            throw SyncException("task-read-only", "Edit this delegated task in webmail.", false);
    }
    if (operation == "delete") {
        if (id.empty()) throw SyncException("task-id-required", "Select a task to delete.", false);
        return requestJSON("/tasks/delete", "POST", json::array({{{"sourceOwner", owner}, {"sourceId", sourceId}, {"id", id}}}));
    }
    const auto changes = request.at("changes");
    for (const char * key : {"subject", "description", "due", "start", "useDateTime", "priority", "status", "percentComplete"})
        if (changes.count(key)) task[key] = changes[key];
    if (!task["subject"].is_string() || task["subject"].get<string>().empty())
        throw SyncException("task-title-required", "Enter a task title.", false);
    const auto saved = requestJSON("/tasks/save", "POST", json::array({task}));
    if (!saved.is_array() || saved.empty() || saved.at(0).value("id", string()).empty())
        throw SyncException("task-save-unconfirmed", "The server did not confirm the saved task. Refresh before retrying.", false);
    return saved;
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
        } catch (SyncException & ex) {
            if (ex.key != "smartermail-http-404" && ex.key != "smartermail-http-405" &&
                ex.key != "smartermail-http-400") throw;
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
    if (ambiguousEmpty) {
        // This server returns [] on a successful delete, but its detail route
        // returns 500 for a missing event. Verify absence using a successful full
        // snapshot instead; never reinterpret a failed detail request as absence.
        for (const auto & event : calendarEvents(calendarSources())) {
            string folder = event.value("calId", event.value("calendarId", ""));
            string uid = event.value("uid", "");
            if (folder == calendarId && uid == eventId) {
                throw SyncException("smartermail-calendar-delete-noop",
                    "SmarterMail returned an empty delete response and the event still exists.", true);
            }
        }
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
