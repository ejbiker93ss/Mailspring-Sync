#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include "SmarterMailHeaders.hpp"
#include "SmarterMailRawMessage.hpp"
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
    const std::string escapedFragment =
        "&lt;div dir=\"auto\"&gt;&lt;span&gt;Thank you&lt;/span&gt;&lt;br&gt;"
        "&lt;div&gt;I will follow up.&lt;/div&gt;&lt;/div&gt;";
    const auto fragment = render("text/plain", escapedFragment);
    assert(!fragment.first->_data.value("plaintext", true));
    assert(fragment.second ==
        "<div dir=\"auto\"><span>Thank you</span><br><div>I will follow up.</div></div>");
    assert(fragment.first->_data.value("snippet", "") == "Thank you I will follow up.");
    const auto bare = render("text/plain", html);
    assert(!bare.first->_data.value("plaintext", true));
    assert(bare.second == html);
    assert(bare.first->_data.value("snippet", "") == "Offer & savings");
    const auto healthy = render("text/html", html);
    assert(healthy.second == html);
    // Optional original-image recovery can produce a complete MIME envelope
    // whose only rendered content is its closing boundary. Reject that source
    // without overwriting a readable cached body, then retain API references
    // when the structured display is used as the fallback.
    for (const auto & delimiter : {"b1_714a42de12769362a3cd95f6c67c147e--",
                                  "0000000000009ba7be065df4973d--",
                                  "===============2440510202301742514==--"}) {
        const std::string brokenOriginal = std::string("MIME-Version: 1.0\r\n") +
            "Content-Type: text/plain\r\n\r\n" + delimiter;
        auto brokenParser = MessageParser::messageParserWithData(Data::dataWithBytes(
            brokenOriginal.data(), (unsigned int)brokenOriginal.size()));
        assert(!processor.retrievedMessageBody(healthy.first.get(), brokenParser));
        SQLite::Statement cached(store.db(), "SELECT value FROM MessageBody WHERE id = ?");
        cached.bind(1, healthy.first->id());
        assert(cached.executeStep() && cached.getColumn(0).getString() == html);
    }
    const std::string fallback = SmarterMailHeaders::mime(json{{"header",
        "Message-ID: <child@host>\r\nReferences: <root@host> <parent@host>\r\n"}}) +
        SmarterMailRawMessage::structuredBodyMime(html, "Offer & savings");
    auto fallbackParser = MessageParser::messageParserWithData(Data::dataWithBytes(
        fallback.data(), (unsigned int)fallback.size()));
    assert(processor.retrievedMessageBody(healthy.first.get(), fallbackParser));
    assert(fallbackParser->header()->references()->count() == 2);
    SQLite::Statement fallbackBody(store.db(), "SELECT value FROM MessageBody WHERE id = ?");
    fallbackBody.bind(1, healthy.first->id());
    assert(fallbackBody.executeStep() && fallbackBody.getColumn(0).getString() == html);
    fallbackBody.reset();
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
