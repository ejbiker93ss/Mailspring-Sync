#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include <cassert>
#include <iostream>

int main() {
    spdlog::stdout_logger_mt("logger");
    SetThreadName("test");
    AutoreleasePool pool;
    MailStore store;
    store.migrate();
    auto account = std::make_shared<Account>(json{{"id", "test-sm"}, {"provider", "smartermail"},
        {"settings", {{"smartermail_server", "https://example.invalid"}}}});
    Folder inbox("test-inbox", "test-sm", 0);
    inbox.setPath("Inbox"); inbox.setRole("inbox"); store.save(&inbox);
    MailProcessor processor(account, &store);
    unsigned int uid = 0;
    auto render = [&](const std::string & type, const std::string & body) {
        auto remote = new IMAPMessage(); remote->autorelease(); remote->setUid(++uid);
        remote->header()->setMessageID(String::stringWithUTF8Characters(("body-" + std::to_string(uid)).c_str()));
        remote->header()->setSubject(MCSTR("Body rendering"));
        remote->header()->setDate(1700000000 + uid);
        remote->header()->setReceivedDate(1700000000 + uid);
        remote->header()->setFrom(Address::addressWithMailbox(MCSTR("test@example.com")));
        auto message = processor.insertMessage(remote, inbox, time(0));
        const std::string mime = "MIME-Version: 1.0\r\nContent-Type: " + type +
            "; charset=utf-8\r\n\r\n" + body;
        auto parser = MessageParser::messageParserWithData(Data::dataWithBytes(mime.data(), (unsigned int)mime.size()));
        assert(processor.retrievedMessageBody(message.get(), parser));
        SQLite::Statement cached(store.db(), "SELECT value FROM MessageBody WHERE id = ?");
        cached.bind(1, message->id());
        assert(cached.executeStep());
        return std::make_pair(message, cached.getColumn(0).getString());
    };
    const std::string html = "<!DOCTYPE html><html><head><style>p {color:red}</style></head>"
        "<body><p>Offer &amp; savings</p></body></html>";
    const std::string escaped = "&lt;!DOCTYPE html&gt;&lt;html&gt;&lt;body&gt;"
        "&lt;p&gt;Offer &amp;amp; savings&lt;/p&gt;&lt;/body&gt;&lt;/html&gt;";
    for (const auto & type : {"text/html", "text/plain"}) {
        const auto result = render(type, escaped);
        assert(!result.first->_data.value("plaintext", true));
        assert(result.second.find("<!DOCTYPE html><html><body><p>Offer &amp; savings</p>") == 0);
        assert(result.first->_data.value("snippet", "") == "Offer & savings");
    }
    const auto bare = render("text/plain", html);
    assert(!bare.first->_data.value("plaintext", true));
    assert(bare.second == html);
    assert(bare.first->_data.value("snippet", "") == "Offer & savings");
    const auto healthy = render("text/html", html);
    assert(healthy.second == html);
    const std::string literal = "Use &lt;html&gt; as an example.";
    const auto text = render("text/plain", literal);
    assert(text.first->_data.value("plaintext", false));
    assert(text.second == literal);
    const std::string htmlExample = "<html><body>Use &lt;html&gt; as an example.</body></html>";
    assert(render("text/html", htmlExample).second == htmlExample);
    account->_data["provider"] = "imap";
    const auto otherProvider = render("text/plain", html);
    assert(otherProvider.first->_data.value("plaintext", false));
    assert(otherProvider.second == html);
    std::cout << "PASS: escaped MIME HTML/plain bodies, HTML mislabeled as text, previews, healthy HTML, literals, provider isolation\n";
}
