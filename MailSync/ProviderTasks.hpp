#pragma once
#include "Account.hpp"
#include "SmarterMailClient.hpp"
#include "NetworkRequestUtils.hpp"
#include "XOAuth2TokenManager.hpp"
#include "SyncException.hpp"

// Interactive, bounded task operations. Credentials remain inside the mail engine.
inline json providerTaskOperation(shared_ptr<Account> account, const json & request) {
    if (account->usesSmarterMailAPI()) return SmarterMailClient(account).taskOperation(request);
    if (!account->usesMicrosoftGraph()) throw SyncException("tasks-not-supported", "Tasks require a SmarterMail API or Microsoft 365 account.", false);
    auto encode = [](const string & value) { CURL * c = curl_easy_init(); char * p = curl_easy_escape(c, value.c_str(), (int)value.size()); string s(p); curl_free(p); curl_easy_cleanup(c); return s; };
    const string base = MicrosoftGraphBaseURL(account) + "/todo/lists";
    const string operation = request.at("operation");
    string url = base, method = "GET";
    json payload = nullptr;
    if (operation != "sources") {
        url += "/" + encode(request.at("sourceId")) + "/tasks";
        const string id = request.value("taskId", string());
        if (!id.empty()) url += "/" + encode(id);
        if (operation == "save") {
            method = id.empty() ? "POST" : "PATCH";
            payload = json::object();
            for (const char * key : {"title", "body", "status", "importance", "dueDateTime"})
                if (request.at("changes").count(key)) payload[key] = request["changes"][key];
        } else if (operation == "delete" && !id.empty()) method = "DELETE";
        else if (operation != "list" && operation != "detail") throw SyncException("invalid-task-operation", "Unsupported task operation.", false);
    }
    if (operation == "sources" || operation == "list") {
        const string next = request.value("next", string());
        if (!next.empty()) {
            // Never send the bearer token to an arbitrary pagination URL.
            if (next.compare(0, url.size() + 1, url + "?") != 0)
                throw SyncException("invalid-task-page", "Invalid task page. Refresh the list.", false);
            url = next;
        } else url += "?$top=100";
    }
    const auto token = SharedXOAuth2TokenManager()->partsForAccount(account).accessToken;
    const string serialized = payload.is_null() ? "" : payload.dump();
    CURL * curl = CreateMicrosoftGraphRequest(url, method, token, serialized.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    const auto response = PerformRequestWithStatus(curl);
    if (response.status == 403) throw SyncException("tasks-permission-required", "Reconnect this Microsoft account and grant Tasks.ReadWrite permission.", false);
    if (response.status < 200 || response.status >= 300) throw SyncException("tasks-request-failed", "Task request failed (HTTP " + to_string(response.status) + "). Refresh before retrying a change.", false);
    return response.body.empty() ? json::object() : json::parse(response.body);
}
