#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include <cassert>
#include <iostream>

int main() {
    spdlog::stdout_logger_mt("logger"); SetThreadName("test"); AutoreleasePool pool;
    MailStore store; store.migrate();
    auto account = std::make_shared<Account>(json{{"id", "copies"}, {"provider", "imap"}, {"settings", {{"imap_host", "example.invalid"}}}});
    Folder inbox("inbox", account->id(), 0), sent("sent", account->id(), 0);
    inbox.setPath("INBOX"); inbox.setRole("inbox"); sent.setPath("Sent Items"); sent.setRole("sent");
    store.save(&inbox); store.save(&sent); MailProcessor processor(account, &store);
    auto remote = new IMAPMessage(); remote->autorelease();
    remote->header()->setMessageID(MCSTR("same@example.invalid"));
    remote->header()->setSubject(MCSTR("Screenshot")); remote->header()->setDate(1700000000);
    remote->header()->setFrom(Address::addressWithMailbox(MCSTR("me@example.invalid")));
    remote->setUid(10);
    auto a = processor.insertFallbackToUpdateMessage(remote, sent, time(0));
    remote->setUid(20);
    auto b = processor.insertFallbackToUpdateMessage(remote, inbox, time(0));
    assert(a->id() != b->id()); assert(a->threadId() == b->threadId());
    for (int i = 0; i < 3; ++i) {
        remote->setUid(10); processor.insertFallbackToUpdateMessage(remote, sent, time(0));
        remote->setUid(20); processor.insertFallbackToUpdateMessage(remote, inbox, time(0));
    }
    a = store.find<Message>(Query().equal("id", a->id()));
    b = store.find<Message>(Query().equal("id", b->id()));
    assert(a->remoteFolderId() == sent.id() && a->remoteUID() == 10);
    assert(b->remoteFolderId() == inbox.id() && b->remoteUID() == 20);
    assert(store.findAll<Message>(Query().equal("accountId", account->id())).size() == 2);
    std::cout << "PASS: Inbox and Sent copies stay separate through repeated sync, sharing a thread\n";
}
