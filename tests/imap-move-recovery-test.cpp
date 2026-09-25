#include "TaskProcessor.hpp"
#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include "constants.h"
#include <cassert>
#include <iostream>
#include "MoveResult.hpp"

int main(int argc, char ** argv) {
    assert(argc == 3);
    spdlog::stdout_logger_mt("logger");
    SetThreadName("test");
    AutoreleasePool pool;
    MailStore store;
    store.migrate();
    auto account = std::make_shared<Account>(json{{"id", "move-test"}, {"provider", "imap"},
        {"settings", {{"imap_host", "127.0.0.1"}}}});
    Folder first("first", account->id(), 0), second("second", account->id(), 0), dest("dest", account->id(), 0);
    first.setPath("A"); second.setPath("B"); dest.setPath("Destination");
    first.setRole("inbox"); second.setRole(""); dest.setRole("");
    store.save(&first); store.save(&second); store.save(&dest);
    MailProcessor processor(account, &store);
    auto insert = [&](Folder & folder, unsigned int uid) {
        auto remote = new IMAPMessage(); remote->autorelease(); remote->setUid(uid);
        remote->header()->setMessageID(AS_MCSTR(folder.path()));
        remote->header()->setSubject(MCSTR("move test"));
        remote->header()->setDate(1700000000 + uid);
        remote->header()->setFrom(Address::addressWithMailbox(MCSTR("test@example.invalid")));
        return processor.insertMessage(remote, folder, time(0));
    };
    auto a = insert(first, 1), b = insert(second, 2);
    Folder sent("sent", account->id(), 0);
    sent.setPath("Custom Sent Folder"); sent.setRole("sent"); store.save(&sent);
    auto reply = insert(sent, 4);
    reply->setThreadId(a->threadId()); store.save(reply.get());
    IMAPSession session;
    session.setHostname(MCSTR("127.0.0.1")); session.setPort(atoi(argv[1]));
    session.setUsername(MCSTR("test")); session.setPassword(MCSTR("test"));
    session.setConnectionType(ConnectionTypeClear); session.setAuthType(AuthTypeSASLNone);
    ErrorCode err = ErrorNone; session.connect(&err); assert(err == ErrorNone);
    TaskProcessor tasks(account, &store, &session);
    Task task("ChangeFolderTask", account->id(), {{"threadIds", {a->threadId(), b->threadId()}}, {"folder", dest.toJSON()}, {"preserveSent", true}});
    tasks.performLocal(&task);
    assert(task.data()["resolvedMessageIds"].size() == 2);
    reply = store.find<Message>(Query().equal("id", reply->id()));
    assert(reply->clientFolderId() == sent.id() && reply->syncUnsavedChanges() == 0);
    const auto oldThread = a->threadId();
    auto late = insert(first, 3);
    late->setThreadId(oldThread); store.save(late.get());
    // Repair thread membership after optimistic apply. The original messages
    // must still move, but a newly arriving reply must stay in the inbox.
    a = store.find<Message>(Query().equal("id", a->id()));
    a->setThreadId("repaired-thread"); store.save(a.get());
    tasks.performRemote(&task);
    const std::string mode = argv[2];
    const auto error = task.toJSON()["error"];
    if (mode == "stale") {
        assert(error.is_null());
    } else {
        assert(!error.is_null());
        if (mode == "noop") assert(error["key"] == "move-incomplete");
        if (mode == "copy") assert(error["debuginfo"] == "moveMessages(mark deleted after copy)");
    }
    a = store.find<Message>(Query().equal("id", a->id()));
    b = store.find<Message>(Query().equal("id", b->id()));
    assert(a->syncUnsavedChanges() == 0 && b->syncUnsavedChanges() == 0);
    if (mode == "stale") {
        assert(a->clientFolderId() == dest.id() && a->remoteUID() == 101);
        assert(b->clientFolderId() == dest.id() && b->remoteUID() == 142);
    } else {
        assert(b->clientFolderId() == second.id() && b->remoteUID() == 2);
        assert(b->syncedAt() == 0);
    }
    late = store.find<Message>(Query().equal("id", late->id()));
    assert(late->remoteUID() == 3 && late->clientFolderId() == first.id());
    assert(late->syncUnsavedChanges() == 0);
    reply = store.find<Message>(Query().equal("id", reply->id()));
    assert(reply->clientFolderId() == sent.id() && reply->remoteFolderId() == sent.id());
    assert(reply->remoteUID() == 4 && reply->syncUnsavedChanges() == 0);
    Task onlySent("ChangeFolderTask", account->id(), {{"messageIds", {reply->id()}}, {"folder", dest.toJSON()}, {"preserveSent", true}});
    tasks.performLocal(&onlySent); tasks.performRemote(&onlySent);
    assert(onlySent.data()["resolvedMessageIds"].empty());
    assert(onlySent.toJSON()["error"].is_null());
    Task explicitMove("ChangeFolderTask", account->id(), {{"messageIds", {reply->id()}}, {"folder", dest.toJSON()}});
    tasks.performLocal(&explicitMove);
    reply = store.find<Message>(Query().equal("id", reply->id()));
    assert(reply->clientFolderId() == dest.id()); // Move-to-folder remains explicit.
    if (mode == "partial" || mode == "stale") {
        assert(a->remoteUID() == 101 && a->remoteFolderId() == dest.id());
        assert(a->clientFolderId() == dest.id());
    } else {
        assert(a->remoteUID() == 1 && a->clientFolderId() == first.id());
        assert(a->syncedAt() == 0);
    }
    auto sm = std::make_shared<Account>(json{{"id", account->id()}, {"provider", "smartermail"},
        {"settings", {{"smartermail_server", "https://unused.invalid"}}}});
    late->setRemoteUID(UINT32_MAX - 1); store.save(late.get());
    TaskProcessor smTasks(sm, &store, nullptr);
    Task unresolved("ChangeFolderTask", sm->id(), {{"messageIds", {late->id()}}, {"folder", dest.toJSON()}});
    smTasks.performLocal(&unresolved);
    smTasks.performRemote(&unresolved); // Must fail without any network request.
    assert(unresolved.toJSON()["error"]["key"] == "move-unconfirmed");
    late = store.find<Message>(Query().equal("id", late->id()));
    assert(late->clientFolderId() == first.id() && late->syncUnsavedChanges() == 0 && late->syncedAt() == 0);
    assert(MoveResult::mapped(json{{"destinationUIDs", {{"1", "101"}}}}, 1) == 101);
    assert(MoveResult::mapped(json::array({{{"uid", 2}, {"newUid", 102}}}), 2) == 102);
    assert(MoveResult::mapped(json{{"destinationUIDs", {101, 102}}}, 1) == 0);
    assert(MoveResult::mapped(json{{"destinationUIDs", {{"1", UINT32_MAX - 1}}}}, 1) == 0);
    const vector<json> rows = {{{"uid", 101}, {"messageId", "<test@example.invalid>"}}};
    assert(MoveResult::newlyObserved(rows, {}, "test@example.invalid") == 101);
    assert(MoveResult::newlyObserved(rows, {101}, "test@example.invalid") == 0);
    assert(MoveResult::newlyObserved(rows, {}, "different@example.invalid") == 0);
    assert(MoveResult::sourceMayStillBeStale(100, 219));
    assert(!MoveResult::sourceMayStillBeStale(100, 220));
    assert(!MoveResult::sourceMayStillBeStale(0, 100));
    std::cout << "PASS: archive preserves Sent copies; Sent-only archive is a no-op; explicit moves remain supported\n";
    std::cout << "PASS: thread snapshot survives repair; late replies stay; confirmed progress survives; failures and unresolved API IDs restore; safe UID mapping\n";
    if (mode == "stale") std::cout << "PASS: stale IMAP UID is recovered by targeted Message-ID search and retried once\n";
}
