#include "ReplyHeaders.hpp"
#include "SmarterMailCompose.hpp"
#include "SmarterMailHeaders.hpp"
#include "MailProcessor.hpp"
#include "ThreadUtils.h"
#include <cassert>
#include <iostream>
using namespace mailcore;
using namespace nlohmann;

int main() {
  try {
    spdlog::stdout_logger_mt("logger"); SetThreadName("test"); AutoreleasePool pool;
    MailStore store; store.migrate();
    // Reuse the normal insertion path so cache identity and MIME serialization
    // are exercised together, not just a string-building imitation.
    Folder folder("inbox", "account", 0); folder.setPath("INBOX"); folder.setRole("inbox");
    store.save(&folder);
    auto remote = new IMAPMessage(); remote->autorelease(); remote->setUid(42);
    remote->header()->setMessageID(MCSTR("smartermail-INBOX-42"));
    remote->header()->setSubject(MCSTR("Ticket")); remote->header()->setDate(1700000000);
    remote->header()->setFrom(Address::addressWithMailbox(MCSTR("support@example.invalid")));
    auto account = std::make_shared<Account>(json{{"id", "account"}, {"provider", "imap"}, {"settings", {{"imap_host", "example.invalid"}}}});
    MailProcessor processor(account, &store);
    Message parent = *processor.insertMessage(remote, folder, time(0));
    auto draftData = parent.toJSON(); draftData["rthMsgId"] = "smartermail-INBOX-42"; draftData["replyToMessageId"] = parent.id();
    Message draft(draftData);
    // The server-managed API path must not copy the HTML-escaped cached MID
    // from the reported failure into outgoing RFC headers.
    draft._data["rthMsgId"] = "&lt;Ticket.CASE@HelpDesk&gt;";
    auto native = SmarterMailCompose::payload(draft, "me@example.invalid", "reply body", true, &parent);
    assert(native["replyUid"].is_number_unsigned() && native["replyUid"] == 42);
    assert(native["replyFromFolder"] == "INBOX" && native["replyOwner"] == "me@example.invalid");
    assert(native["actions"]["Replied"] == true && native["isReply"] == true);
    assert(native["messagePlainText"] == "reply body" && native["messageHTML"] == "");
    bool missingParentRejected = false;
    try { SmarterMailCompose::payload(draft, "me@example.invalid", "reply", true, nullptr); }
    catch (const SyncException &) { missingParentRejected = true; }
    assert(missingParentRejected);
    for (const char * key : {"headers", "messageId", "Message-ID", "inReplyTo", "references", "In-Reply-To", "References"}) assert(!native.count(key));
    assert(SmarterMailHeaders::ids("&lt;Ticket.CASE@HelpDesk&gt;") == "<Ticket.CASE@HelpDesk> ");
    const auto guid = SmarterMailCompose::guid();
    assert(guid.size() == 36 && guid[14] == '4' && guid[8] == '-');
    draft._data["rthMsgId"] = "smartermail-INBOX-42";
    auto parse = [](const std::string & raw) {return MessageParser::messageParserWithData(Data::dataWithBytes(raw.data(), (unsigned int)raw.size()))->header();};
    int fetches = 0;
    auto fetch = [&](Message * target) {
        assert(target->remoteUID() == 42); ++fetches;
        return parse("Message-ID: <Parent.CASE@HelpDesk>\r\nReferences: <Ticket.ROOT@HelpDesk> <Middle@Host>\r\n\r\n");
    };
    MessageBuilder builder; builder.header()->setMessageID(MCSTR("New.Unique@SummerMail")); builder.setTextBody(MCSTR("Reply"));
    ReplyHeaders::apply(builder.header(), draft, &store, true, fetch);
    assert(fetches == 1);
    auto header = MessageParser::messageParserWithData(builder.data())->header();
    assert(std::string(header->messageID()->UTF8Characters()) == "New.Unique@SummerMail");
    assert(std::string(((String *)header->inReplyTo()->objectAtIndex(0))->UTF8Characters()) == "Parent.CASE@HelpDesk");
    assert(header->references()->count() == 3);
    assert(std::string(((String *)header->references()->objectAtIndex(0))->UTF8Characters()) == "Ticket.ROOT@HelpDesk");
    // The UI still has the placeholder, but the stable local identity resolves
    // the now-hydrated parent without another network request.
    ReplyHeaders::apply(builder.header(), draft, &store, true, fetch); assert(fetches == 1);
    auto old = store.find<Message>(Query().equal("id", parent.id()));
    old->_data.erase("references"); store.save(old.get());
    ReplyHeaders::apply(builder.header(), draft, &store, false, fetch); assert(fetches == 2);
    old = store.find<Message>(Query().equal("id", parent.id()));
    old->_data["references"] = json::array(); old->_data["rthMsgId"] = "<RootOnly@Host>"; store.save(old.get());
    ReplyHeaders::apply(builder.header(), draft, &store, false, fetch);
    assert(builder.header()->references()->count() == 2); assert(fetches == 2);
    assert(std::string(((String *)builder.header()->references()->objectAtIndex(0))->UTF8Characters()) == "RootOnly@Host");
    draftData.erase("replyToMessageId"); draftData["rthMsgId"] = "smartermail-missing-1";
    Message invalid(draftData);
    bool rejected = false;
    try {ReplyHeaders::apply(builder.header(), invalid, &store, false, fetch);} catch (const SyncException &) {rejected = true;}
    assert(rejected);
    draftData["rthMsgId"] = nullptr; draftData["replyToMessageId"] = nullptr;
    Message newDraft(draftData); MessageBuilder newBuilder;
    ReplyHeaders::apply(newBuilder.header(), newDraft, &store, false, fetch);
    assert(!newBuilder.header()->inReplyTo() || newBuilder.header()->inReplyTo()->count() == 0);
    assert(fetches == 2);
    auto fresh = SmarterMailCompose::payload(newDraft, "me@example.invalid", "<b>new</b>", false, nullptr);
    assert(!fresh.count("actions") && !fresh.count("replyUid") && fresh["messageHTML"] == "<b>new</b>");
    draft._data["fwdMsgId"] = "parent@host";
    auto forward = SmarterMailCompose::payload(draft, "me@example.invalid", "forward", true, &parent);
    assert(forward["actions"]["Forwarded"] == true && !forward.count("isReply"));
    assert(ReplyHeaders::canonical("<Parent.CASE@HelpDesk>") == "Parent.CASE@HelpDesk");
    assert(ReplyHeaders::canonical("parent@host\r\nBcc: victim@host").empty());
    std::cout << "PASS: serialized reply MIME preserves exact parent, full ticket references, fresh ID; stale caches heal and placeholders cannot send\n";
    std::cout << "PASS: native SmarterMail payload uses numeric parent UID, folder, owner and reply/forward action, with no RFC header overrides\n";
  } catch (const std::exception & ex) { std::cerr << ex.what() << std::endl; return 1; }
}
