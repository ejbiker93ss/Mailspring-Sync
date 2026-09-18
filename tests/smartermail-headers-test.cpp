#include "../MailSync/SmarterMailHeaders.hpp"
#include <cassert>
int main() {
    using nlohmann::json;
    using namespace SmarterMailHeaders;
    assert(ids("<Case@Example> <Case@Example> <parent@host>") == "<Case@Example> <parent@host> ");
    assert(ids("1234").empty());
    const json message = {{"internetMessageId", "<child@host>"},
        {"references", json::array({"<root@host>", "<parent@host>"})},
        {"inReplyToHeader", "<parent@host>"}};
    const auto headers = mime(message);
    assert(headers.find("Message-ID: <child@host>\r\n") == 0);
    assert(headers.find("References: <root@host> <parent@host> \r\n") != std::string::npos);
    assert(mime(json{{"messageID", 42}}).empty());
    const json nested = {{"additionalHeaders", json::array({
        {{"name", "Message-ID"}, {"value", "<Case@host>"}},
        {{"name", "In-Reply-To"}, {"value", "<root@host>\r\n\t<parent@host>"}}
    })}};
    assert(mime(nested).find("Message-ID: <Case@host>") == 0);
    assert(mime(nested).find("References: <root@host> <parent@host>") != std::string::npos);
    assert(ids("<valid@host>\r\nBcc: victim").find("Bcc") == std::string::npos);
}
