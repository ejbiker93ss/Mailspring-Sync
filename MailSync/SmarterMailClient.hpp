#ifndef SmarterMailClient_hpp
#define SmarterMailClient_hpp

#include <memory>
#include <string>
#include <vector>

#include "json.hpp"

class Account;

struct SmarterMailPage {
    std::vector<nlohmann::json> messages;
    unsigned int totalCount = 0;
    bool hasTotalCount = false;
};

class SmarterMailClient {
public:
    explicit SmarterMailClient(std::shared_ptr<Account> account);

    void validate();
    std::vector<nlohmann::json> folders();
    SmarterMailPage messages(const std::string & folder, unsigned int skip, unsigned int take);
    std::string rawMessage(const std::string & folder, uint32_t uid);

    void markRead(const std::string & folder, const std::vector<uint32_t> & uids, bool read);
    void setFlagged(const std::string & folder, const std::vector<uint32_t> & uids, bool flagged);
    nlohmann::json move(const std::string & folder, const std::vector<uint32_t> & uids,
                        const std::string & destinationFolder);
    void remove(const std::string & folder, const std::vector<uint32_t> & uids, bool moveToDeleted = true);
    void createFolder(const std::string & path);
    void renameFolder(const std::string & oldPath, const std::string & newPath);
    void deleteFolder(const std::string & path);
    void importMime(const std::string & folder, const std::string & mime);

private:
    std::shared_ptr<Account> account;
    std::string baseUrl;

    void authenticate(bool force = false);
    nlohmann::json requestJSON(const std::string & path, const std::string & method = "GET",
                               const nlohmann::json & payload = nullptr);
    std::string requestText(const std::string & path, const nlohmann::json & payload);
    static void requireSuccess(const nlohmann::json & response, const std::string & operation);
};

#endif
