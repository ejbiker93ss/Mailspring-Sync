#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include "SmarterMailHeaders.hpp"
#include <cassert>
#include <iostream>

int main() {
    spdlog::stdout_logger_mt("logger");
    SetThreadName("test");
    AutoreleasePool pool;
    MailStore store;
    store.migrate();
    auto account = std::make_shared<Account>(json{{"id", "test-sm"}, {"provider", "smartermail"}, {"settings", {{"smartermail_server", "https://example.invalid"}}}});
    assert(account->usesSmarterMailAPI());
    Folder inbox("test-inbox", "test-sm", 0);
    inbox.setPath("Inbox"); inbox.setRole("inbox"); store.save(&inbox);
    MailProcessor processor(account, &store);
    const auto nativeHeaders = SmarterMailHeaders::mime(json{{"header",
        "Message-Id: <sent@host>\r\nIn-Reply-To: <parent@host>\r\nReferences: <root@host>\r\n\t<parent@host>\r\nContent-Type: multipart/alternative; boundary=abc\r\n\r\nReferences: <not-a-header@host>"}});
    assert(nativeHeaders.find("References: <root@host> <parent@host>") != std::string::npos);
    assert(nativeHeaders.find("Message-ID: <sent@host>") != std::string::npos);
    assert(nativeHeaders.find("Content-Type") == std::string::npos);
    assert(nativeHeaders.find("not-a-header") == std::string::npos);
    auto insert = [&](unsigned int uid, const char * id) {
        auto m = new IMAPMessage(); m->autorelease(); m->setUid(uid);
        m->header()->setMessageID(String::stringWithUTF8Characters(id));
        m->header()->setSubject(MCSTR("Re: Same subject"));
        m->header()->setDate(1700000000 + uid); m->header()->setReceivedDate(1700000000 + uid);
        m->header()->setFrom(Address::addressWithMailbox(MCSTR("test@example.com")));
        return processor.insertMessage(m, inbox, time(0));
    };
    auto a = insert(1, "placeholder-a");
    auto b = insert(2, "placeholder-b");
    auto unrelated = insert(3, "unrelated");
    const auto aid = a->id(), bid = b->id();
    auto heal = [&](Message * m, const char * mid, const char * refs) {
        auto mime = SmarterMailHeaders::mime(json{{"header", std::string("Message-Id: ") + mid + "\r\nReferences: " + refs + "\r\n"}}) + "Content-Type: text/plain\r\n\r\nHello preview";
        auto parser = MessageParser::messageParserWithData(Data::dataWithBytes(mime.data(), (unsigned int)mime.size()));
        processor.retrievedMessageBody(m, parser);
        processor.repairSmarterMailThread(m, parser->header());
    };
    heal(a.get(), "<root@host>", "");
    heal(b.get(), "<child@host>", "<root@host>");
    auto currentA = store.find<Message>(Query().equal("id", aid));
    auto currentB = store.find<Message>(Query().equal("id", bid));
    assert(currentA->threadId() == currentB->threadId());
    assert(currentA->threadId() != unrelated->threadId());
    heal(b.get(), "<child@host>", "<root@host>");
    auto thread = store.find<Thread>(Query().equal("id", currentB->threadId()));
    assert(thread->unread() == 2);
    assert(store.findAll<Message>(Query().equal("accountId", "test-sm")).size() == 3);
    SQLite::Statement bodies(store.db(), "SELECT COUNT(*) FROM MessageBody");
    assert(bodies.executeStep() && bodies.getColumn(0).getInt() == 2);
    // Server-managed replies may omit RFC refs in JSON but expose their exact
    // original-message pointer. Keep physical Sent identity while linking it.
    Folder sent("test-sent", "test-sm", 0); sent.setPath("Sent Items"); sent.setRole("sent"); store.save(&sent);
    auto reply = insert(4, "&lt;sent@host&gt;");
    reply->setRemoteFolder(&sent); reply->setClientFolder(&sent); store.save(reply.get());
    auto sparse = new MessageHeader(); sparse->autorelease(); sparse->setMessageID(MCSTR("&lt;sent@host&gt;"));
    processor.repairSmarterMailThread(reply.get(), sparse, aid);
    auto linked = store.find<Message>(Query().equal("id", reply->id()));
    auto linkedParent = store.find<Message>(Query().equal("id", aid));
    assert(linked->threadId() == linkedParent->threadId());
    assert(linked->remoteFolderId() == sent.id() && linked->remoteUID() == 4);
    assert(linked->headerMessageId() == "sent@host");
    assert(linked->threadId() != unrelated->threadId());
    // Sparse follow-up reads must not clear previously recovered RFC refs.
    auto child = store.find<Message>(Query().equal("id", bid));
    sparse->setMessageID(MCSTR("child@host"));
    processor.repairSmarterMailThread(child.get(), sparse);
    child = store.find<Message>(Query().equal("id", bid));
    assert(child->_data["references"].size() == 1);
    std::cout << "PASS: existing singleton merge, idempotence, stable IDs, counts, body retention, no subject-only merge\n";
    std::cout << "PASS: server parent pointer links Sent copy; escaped IDs normalize; sparse reads preserve refs\n";
}
