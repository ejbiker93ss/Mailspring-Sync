#include "SmarterMailDataWorker.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

#include "Calendar.hpp"
#include "ContactBook.hpp"
#include "ContactGroup.hpp"
#include "DAVUtils.hpp"
#include "MailStore.hpp"
#include "MailStoreTransaction.hpp"
#include "MailUtils.hpp"
#include "SmarterMailClient.hpp"
#include "SyncException.hpp"
#include "VCard.hpp"
#include "icalendar.h"
#include "spdlog/spdlog.h"

using namespace nlohmann;
using namespace std;

namespace {
const string BOOK_SUFFIX = "-smartermail-contacts";

string firstString(const json & value, initializer_list<const char *> keys, const string & fallback = "") {
    if (!value.is_object()) return fallback;
    for (const char * key : keys) {
        if (!value.count(key) || value[key].is_null()) continue;
        if (value[key].is_string()) {
            string result = value[key].get<string>();
            if (!result.empty()) return result;
        } else if (value[key].is_number_integer()) {
            return to_string(value[key].get<long long>());
        }
    }
    return fallback;
}

bool firstBool(const json & value, initializer_list<const char *> keys, bool fallback = false) {
    for (const char * key : keys) {
        if (!value.count(key) || value[key].is_null()) continue;
        if (value[key].is_boolean()) return value[key].get<bool>();
        if (value[key].is_number_integer()) return value[key].get<int>() != 0;
    }
    return fallback;
}

string lowerCopy(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
    return value;
}

string vcardEscape(string value) {
    string out;
    for (char c : value) {
        if (c == '\\' || c == ';' || c == ',') out += '\\';
        if (c == '\r') continue;
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

string vcardUnescape(string value) {
    string out;
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            out += (c == 'n' || c == 'N') ? '\n' : c;
            escaped = false;
        } else if (c == '\\') escaped = true;
        else out += c;
    }
    if (escaped) out += '\\';
    return out;
}

vector<string> vcardPropertyValues(const string & vcf, const string & property) {
    vector<string> result;
    istringstream stream(vcf);
    string line;
    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        string name = line.substr(0, line.find(';'));
        if (lowerCopy(name) == lowerCopy(property)) result.push_back(vcardUnescape(line.substr(colon + 1)));
    }
    return result;
}

string icsEscape(string value) {
    string out;
    for (char c : value) {
        if (c == '\\' || c == ';' || c == ',') out += '\\';
        if (c == '\r') continue;
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

vector<string> stringValues(const json & value, initializer_list<const char *> keys) {
    vector<string> result;
    for (const char * key : keys) {
        if (!value.count(key) || value[key].is_null()) continue;
        const json & raw = value[key];
        if (raw.is_string()) result.push_back(raw.get<string>());
        if (!raw.is_array()) continue;
        for (const auto & item : raw) {
            if (item.is_string()) result.push_back(item.get<string>());
            else if (item.is_object()) {
                string text = firstString(item, {"emailAddress", "email", "address", "number", "phoneNumber", "value", "id"});
                if (!text.empty()) result.push_back(text);
            }
        }
    }
    return result;
}

bool isGroup(const json & raw) {
    if (firstBool(raw, {"isGroup", "IsGroup"})) return true;
    if (raw.value("contactType", 0) == 2 || raw.value("ContactType", 0) == 2) return true;
    string kind = lowerCopy(firstString(raw, {"kind", "contactKind", "type"}));
    return kind == "group" || kind == "distributionlist";
}

string contactId(const json & raw) {
    return firstString(raw, {"uid", "contactUid", "contactUID", "id", "ID"});
}

string contactName(const json & raw) {
    string value = firstString(raw, {"displayAs", "fullName", "displayName", "name"});
    if (!value.empty()) return value;
    string first = firstString(raw, {"firstName", "givenName"});
    string last = firstString(raw, {"lastName", "surname"});
    return first + ((!first.empty() && !last.empty()) ? " " : "") + last;
}

string makeVCard(const json & raw, const string & uid) {
    string name = contactName(raw);
    string first = firstString(raw, {"firstName", "givenName"});
    string last = firstString(raw, {"lastName", "surname"});
    ostringstream out;
    out << "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:" << vcardEscape(uid) << "\r\n";
    out << "FN:" << vcardEscape(name) << "\r\n";
    out << "N:" << vcardEscape(last) << ";" << vcardEscape(first) << ";;;\r\n";
    for (const string & email : stringValues(raw, {"emailAddressList", "emailAddresses", "emails", "emailAddress", "email"}))
        if (!email.empty()) out << "EMAIL:" << vcardEscape(email) << "\r\n";
    for (const string & phone : stringValues(raw, {"phoneNumberList", "phoneNumbers", "phones", "phoneNumber", "phone"}))
        if (!phone.empty()) out << "TEL:" << vcardEscape(phone) << "\r\n";
    string company = firstString(raw, {"company", "companyName", "organization"});
    string title = firstString(raw, {"jobTitle", "title"});
    string note = firstString(raw, {"additionalInfo", "notes", "note"});
    if (!company.empty()) out << "ORG:" << vcardEscape(company) << "\r\n";
    if (!title.empty()) out << "TITLE:" << vcardEscape(title) << "\r\n";
    if (!note.empty()) out << "NOTE:" << vcardEscape(note) << "\r\n";
    if (isGroup(raw)) {
        out << "KIND:group\r\nX-ADDRESSBOOKSERVER-KIND:group\r\n";
        for (const string & member : stringValues(raw, {"groupedContacts", "members"}))
            if (!member.empty()) out << "MEMBER:urn:uuid:" << vcardEscape(member) << "\r\n";
    }
    out << "END:VCARD\r\n";
    return out.str();
}

string sourceOwner(const json & raw, const string & fallback) {
    return firstString(raw, {"sourceOwner", "owner", "ownerUsername", "ownerEmailAddress", "calendarOwner"}, fallback);
}

string sourceId(const json & raw, const string & fallback) {
    return firstString(raw, {"sourceId", "itemID", "ItemID", "folder", "calendarId", "calId"}, fallback);
}

string fullOwner(string owner, const string & email) {
    if (owner.empty()) return email;
    if (owner.find('@') == string::npos) {
        size_t at = email.find('@');
        if (at != string::npos) owner += email.substr(at);
    }
    return owner;
}

string compactDate(const json & raw) {
    if (raw.is_object()) {
        for (const char * key : {"dt", "dateTime", "date_local", "date", "value"})
            if (raw.count(key)) return compactDate(raw[key]);
        return "";
    }
    if (raw.is_number()) {
        time_t stamp = (time_t)raw.get<long long>();
        if (stamp > 100000000000LL) stamp /= 1000;
        tm utc {};
#ifdef _WIN32
        gmtime_s(&utc, &stamp);
#else
        gmtime_r(&stamp, &utc);
#endif
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
        return buffer;
    }
    if (!raw.is_string()) return "";
    string value = raw.get<string>();
    string digits;
    for (char c : value) if (isdigit((unsigned char)c)) digits += c;
    if (digits.size() < 8) return "";
    string result = digits.substr(0, 8);
    if (digits.size() >= 14) result += "T" + digits.substr(8, 6);
    if (!result.empty() && (value.find('Z') != string::npos || value.find('z') != string::npos)) result += "Z";
    return result;
}

string eventDate(const json & raw, initializer_list<const char *> keys) {
    for (const char * key : keys) if (raw.count(key)) {
        string value = compactDate(raw[key]);
        if (!value.empty()) return value;
    }
    return "";
}

string eventTimezone(const json & raw, initializer_list<const char *> keys) {
    for (const char * key : keys) {
        if (!raw.count(key) || !raw[key].is_object()) continue;
        string timezone = firstString(raw[key], {"time_zone_id", "tz", "timeZone", "timezone"});
        if (!timezone.empty()) return timezone;
    }
    return firstString(raw, {"timeZone", "timezone", "timeZoneId"}, "");
}

string timezoneFromICS(const string & ics) {
    istringstream stream(ics);
    string line;
    while (getline(stream, line)) {
        if (line.rfind("DTSTART;", 0) != 0) continue;
        size_t marker = line.find("TZID=");
        if (marker == string::npos) continue;
        marker += 5;
        size_t end = line.find_first_of(";:\r\n", marker);
        return line.substr(marker, end == string::npos ? string::npos : end - marker);
    }
    return "UTC";
}

string calendarId(const json & raw) {
    return firstString(raw, {"calendarId", "calId", "itemID", "ItemID", "sourceId", "id"});
}

string eventId(const json & raw) {
    // Save/delete routes take the UID, not the list row's numeric id.
    return firstString(raw, {"uid", "eventId", "calendarEventId", "id"});
}

string eventUid(const json & raw) {
    return firstString(raw, {"uid", "calendarEventUid", "eventUid", "id"});
}

string calendarOwnerForWrite(SmarterMailClient & client, shared_ptr<Calendar> calendar,
                             const string & email) {
    // Keep normalized owners for stable local IDs, but round-trip the server's
    // owner for API writes. Some servers reject an expanded email with 403.
    // Resolve against current sources, including for calendars cached by older
    // versions, rather than guessing a username or retrying another mailbox.
    for (const auto & source : client.calendarSources()) {
        const string owner = sourceOwner(source, email);
        if (calendarId(source) == calendar->_data.value("smCalendarId", "") &&
            fullOwner(owner, email) == calendar->_data.value("smOwner", "")) {
            return owner;
        }
    }
    throw SyncException("smartermail-calendar-source-missing", "The calendar is no longer available from SmarterMail.", false);
}

string makeICS(const json & raw, const string & uid) {
    bool allDay = firstBool(raw, {"allDayEvent", "allDay", "isAllDay"});
    string start = eventDate(raw, {"start", "startDate", "startWithTZ", "startUtc"});
    string end = eventDate(raw, {"end", "endDate", "endWithTZ", "endUtc"});
    string timezone = eventTimezone(raw, {"start", "startDate", "startWithTZ"});
    if (start.empty()) throw SyncException("smartermail-calendar-invalid-event", "SmarterMail returned an event without a start date.", true);
    ostringstream out;
    out << "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//SummerMail//SmarterMail API//EN\r\nBEGIN:VEVENT\r\n";
    out << "UID:" << icsEscape(uid) << "\r\n";
    string startValue = start.substr(0, allDay ? 8 : string::npos);
    string endValue = end.substr(0, allDay ? 8 : string::npos);
    if (!allDay && !timezone.empty()) {
        if (!startValue.empty() && startValue.back() == 'Z') startValue.pop_back();
        if (!endValue.empty() && endValue.back() == 'Z') endValue.pop_back();
    }
    out << (allDay ? "DTSTART;VALUE=DATE:" : (timezone.empty() ? "DTSTART:" : "DTSTART;TZID=" + timezone + ":")) << startValue << "\r\n";
    if (!end.empty()) out << (allDay ? "DTEND;VALUE=DATE:" : (timezone.empty() ? "DTEND:" : "DTEND;TZID=" + timezone + ":")) << endValue << "\r\n";
    out << "SUMMARY:" << icsEscape(firstString(raw, {"subject", "title", "name"})) << "\r\n";
    string description = firstString(raw, {"description", "notes", "body"});
    string location = firstString(raw, {"location", "locationName"});
    if (!description.empty()) out << "DESCRIPTION:" << icsEscape(description) << "\r\n";
    if (!location.empty()) out << "LOCATION:" << icsEscape(location) << "\r\n";
    string rrule = firstString(raw, {"rrule", "recurrenceRule"});
    if (rrule.rfind("RRULE:", 0) == 0 || rrule.rfind("rrule:", 0) == 0) rrule = rrule.substr(6);
    if (!rrule.empty()) out << "RRULE:" << rrule << "\r\n";
    for (const string & attendee : stringValues(raw, {"attendees"}))
        if (!attendee.empty()) out << "ATTENDEE:mailto:" << icsEscape(attendee) << "\r\n";
    out << "END:VEVENT\r\nEND:VCALENDAR\r\n";
    return out.str();
}

string isoLocal(const Date & value) {
    string compact = value.operator string();
    if (compact.size() < 8) return "";
    string out = compact.substr(0, 4) + "-" + compact.substr(4, 2) + "-" + compact.substr(6, 2);
    if (compact.size() >= 15) out += "T" + compact.substr(9, 2) + ":" + compact.substr(11, 2) + ":" + compact.substr(13, 2);
    return out;
}

json dateEnvelope(const Date & value, const string & timezone) {
    string date = isoLocal(value);
    return {{"dt", date}, {"tz", timezone}, {"date_local", date},
            {"time_zone_id", timezone}, {"has_time", value.WithTime}, {"date", date}};
}
}

SmarterMailDataWorker::SmarterMailDataWorker(shared_ptr<Account> value) : account(value), store(new MailStore()) {}

SmarterMailDataWorker::~SmarterMailDataWorker() { delete store; }

void SmarterMailDataWorker::run() {
    try {
        runContacts();
    } catch (const exception & ex) {
        auto logger = spdlog::get("logger");
        if (logger) logger->warn("SmarterMail contact sync failed ({}); continuing with calendars", ex.what());
    }
    runCalendars();
}

void SmarterMailDataWorker::runContacts() {
    SmarterMailClient client(account);
    vector<json> sources = client.contactSources();
    vector<json> remote;
    const unsigned int pageSize = 200;
    for (unsigned int skip = 0; skip < 20000; skip += pageSize) {
        auto page = client.contacts(sources, skip, pageSize);
        remote.insert(remote.end(), page.contacts.begin(), page.contacts.end());
        if (page.contacts.size() < pageSize || (page.hasTotalCount && remote.size() >= page.totalCount)) break;
        if (skip + pageSize >= 20000) throw SyncException("smartermail-contact-limit", "SmarterMail contact listing exceeded its safety limit.", true);
    }

    string bookId = account->id() + BOOK_SUFFIX;
    auto book = store->find<ContactBook>(Query().equal("id", bookId));
    if (!book) book = make_shared<ContactBook>(bookId, account->id());
    book->setURL(account->smarterMailServer() + "/api/v1/contacts");
    book->setSource(SMARTERMAIL_SYNC_SOURCE);
    book->setVerifiedListing(false);
    store->save(book.get());

    auto local = store->findAllMap<Contact>(Query().equal("accountId", account->id()).equal("bookId", bookId), "id");
    set<string> seen;
    map<string, vector<string>> groupMembers;
    map<string, string> remoteToLocal;
    for (const json & raw : remote) {
        string remoteId = contactId(raw);
        if (remoteId.empty()) continue;
        string localId = MailUtils::idForCalendar(account->id(), "smartermail-contact:" + sourceOwner(raw, "") + ":" + sourceId(raw, "Contacts") + ":" + remoteId);
        remoteToLocal[remoteId] = localId;
        seen.insert(localId);
        shared_ptr<Contact> contact = local.count(localId) ? local[localId] : make_shared<Contact>(localId, account->id(), "", CONTACT_MAX_REFS, SMARTERMAIL_SYNC_SOURCE);
        vector<string> emails = stringValues(raw, {"emailAddressList", "emailAddresses", "emails", "emailAddress", "email"});
        for (const string & email : emails) remoteToLocal[lowerCopy(email)] = localId;
        contact->setName(contactName(raw));
        contact->setEmail(emails.empty() ? "" : emails.front());
        contact->setBookId(bookId);
        contact->setHidden(isGroup(raw));
        contact->setEtag(firstString(raw, {"etag", "lastModified", "dateModified"}));
        contact->setInfo({{"vcf", makeVCard(raw, remoteId)}, {"href", "smartermail:" + remoteId}, {"smartermail", raw},
                          {"sourceOwner", sourceOwner(raw, account->emailAddress())}, {"sourceId", sourceId(raw, "Contacts")}, {"remoteId", remoteId}});
        store->save(contact.get());
        if (isGroup(raw)) groupMembers[localId] = stringValues(raw, {"groupedContacts", "members"});
    }

    for (const auto & entry : groupMembers) {
        auto contact = store->find<Contact>(Query().equal("id", entry.first));
        if (!contact) continue;
        auto group = store->find<ContactGroup>(Query().equal("id", entry.first));
        if (!group) group = make_shared<ContactGroup>(entry.first, account->id());
        group->setName(contact->name());
        group->setBookId(bookId);
        store->save(group.get());
        vector<string> members;
        for (const string & remoteId : entry.second) {
            string key = lowerCopy(remoteId);
            if (remoteToLocal.count(key)) members.push_back(remoteToLocal[key]);
        }
        group->syncMembers(store, members);
    }

    // The list is authoritative only after every page completed successfully.
    for (const auto & entry : local) if (!seen.count(entry.first)) {
        auto group = store->find<ContactGroup>(Query().equal("id", entry.first));
        if (group) store->remove(group.get());
        store->remove(entry.second.get());
    }
    book->setVerifiedListing(true);
    store->save(book.get());

    // A verified native snapshot supersedes the old DAV cache. Removing it
    // only after the complete API listing prevents duplicate contacts while
    // retaining rollback safety on transport or payload failures.
    auto oldBooks = store->findAll<ContactBook>(Query().equal("accountId", account->id()));
    for (auto & oldBook : oldBooks) {
        if (oldBook->id() == bookId || oldBook->source() != CARDDAV_SYNC_SOURCE) continue;
        auto oldContacts = store->findAll<Contact>(Query().equal("accountId", account->id()).equal("bookId", oldBook->id()));
        for (auto & oldContact : oldContacts) {
            auto group = store->find<ContactGroup>(Query().equal("id", oldContact->id()));
            if (group) store->remove(group.get());
            store->remove(oldContact.get());
        }
        store->remove(oldBook.get());
    }
}

void SmarterMailDataWorker::runCalendars() {
    SmarterMailClient client(account);
    vector<json> sources = client.calendarSources();
    vector<json> remoteEvents = client.calendarEvents(sources);
    // Serialize snapshot application with foreground calendar writes. No network
    // work belongs in this transaction; a failed snapshot must not prune events.
    MailStoreTransaction transaction{store, "smartermail:calendars"};
    map<string, shared_ptr<Calendar>> calendars;
    set<string> remoteCalendarIds;
    for (const json & source : sources) {
        string remoteId = calendarId(source);
        if (remoteId.empty()) continue;
        string owner = fullOwner(sourceOwner(source, account->emailAddress()), account->emailAddress());
        string marker = "smartermail-calendar:" + owner + ":" + remoteId;
        string localId = MailUtils::idForCalendar(account->id(), marker);
        remoteCalendarIds.insert(localId);
        auto calendar = store->find<Calendar>(Query().equal("id", localId));
        if (!calendar) calendar = make_shared<Calendar>(localId, account->id());
        calendar->setPath(marker);
        calendar->setName(firstString(source, {"displayName", "name", "calendarName", "sourceName"}, "Calendar"));
        calendar->setColor(firstString(source, {"color", "calendarColor"}));
        string access = lowerCopy(firstString(source, {"access", "accessLevel", "permission"}, "owner"));
        int accessValue = 3;
        for (const char * key : {"access", "accessLevel"}) {
            if (source.count(key) && source[key].is_number_integer()) accessValue = source[key].get<int>();
        }
        calendar->setReadOnly(!(access == "owner" || access == "manage" || access == "write" || accessValue >= 3));
        calendar->_data["smOwner"] = owner;
        calendar->_data["smCalendarId"] = remoteId;
        store->save(calendar.get());
        calendars[owner + "\n" + remoteId] = calendar;
    }

    set<string> seenEvents;
    for (const json & raw : remoteEvents) {
        string remoteCalendar = calendarId(raw);
        string owner = fullOwner(sourceOwner(raw, account->emailAddress()), account->emailAddress());
        auto found = calendars.find(owner + "\n" + remoteCalendar);
        if (found == calendars.end()) {
            for (const auto & entry : calendars) if (entry.second->_data.value("smCalendarId", "") == remoteCalendar) { found = calendars.find(entry.first); break; }
        }
        if (found == calendars.end()) continue;
        string uid = eventUid(raw);
        if (uid.empty()) continue;
        string ics = makeICS(raw, uid);
        ICalendar parsed(ics);
        if (parsed.Events.empty()) continue;
        auto parsedEvent = parsed.Events.front();
        string localId = MailUtils::idForEvent(account->id(), found->second->id(), parsedEvent->UID, parsedEvent->RecurrenceId);
        string etag = firstString(raw, {"etag", "lastModified", "dateModified"});
        // MailStore chooses INSERT vs UPDATE from the model's persisted version.
        // Constructing a fresh Event for an existing ID resets that version and
        // aborts the entire calendar worker with a duplicate-primary-key error.
        auto event = store->find<Event>(Query().equal("id", localId));
        if (event) {
            event->applyICSEventData(etag, uid, ics, parsedEvent);
        } else {
            event = make_shared<Event>(etag, account->id(), found->second->id(), ics, parsedEvent);
        }
        event->setHref(uid);
        event->_data["smOwner"] = owner;
        event->_data["smCalendarId"] = remoteCalendar;
        event->_data["smEventId"] = eventId(raw);
        event->_data["smartermail"] = raw;
        store->save(event.get());
        seenEvents.insert(event->id());
    }

    // events-all2 is authoritative only because the request above succeeded.
    for (const string & localCalendarId : remoteCalendarIds) {
        auto localEvents = store->findAll<Event>(Query().equal("accountId", account->id()).equal("calendarId", localCalendarId));
        for (auto & event : localEvents) if (!seenEvents.count(event->id())) store->remove(event.get());
    }

    // As with contacts, retire legacy CalDAV rows only after sources and the
    // full events-all2 snapshot have both completed successfully.
    if (!sources.empty()) {
        auto localCalendars = store->findAll<Calendar>(Query().equal("accountId", account->id()));
        for (auto & calendar : localCalendars) {
            if (calendar->_data.count("smCalendarId")) continue;
            auto legacyEvents = store->findAll<Event>(Query().equal("accountId", account->id()).equal("calendarId", calendar->id()));
            for (auto & event : legacyEvents) store->remove(event.get());
            store->remove(calendar.get());
        }
    }
    transaction.commit();
    auto logger = spdlog::get("logger");
    if (logger) logger->info("SmarterMail calendar sync complete: {} calendars, {} events", remoteCalendarIds.size(), seenEvents.size());
}

void SmarterMailDataWorker::writeAndResyncContact(shared_ptr<Contact> contact) {
    json info = contact->info();
    json body = info.value("smartermail", json::object());
    string remoteId = info.value("remoteId", "");
    bool created = remoteId.empty();
    string vcf = info.value("vcf", "");
    vector<string> names = vcardPropertyValues(vcf, "N");
    vector<string> emails = vcardPropertyValues(vcf, "EMAIL");
    vector<string> phones = vcardPropertyValues(vcf, "TEL");
    vector<string> orgs = vcardPropertyValues(vcf, "ORG");
    vector<string> titles = vcardPropertyValues(vcf, "TITLE");
    vector<string> notes = vcardPropertyValues(vcf, "NOTE");
    string firstName, lastName;
    if (!names.empty()) {
        size_t split = names.front().find(';');
        lastName = names.front().substr(0, split);
        if (split != string::npos) {
            size_t next = names.front().find(';', split + 1);
            firstName = names.front().substr(split + 1, next == string::npos ? string::npos : next - split - 1);
        }
    }
    if (emails.empty() && !contact->email().empty()) emails.push_back(contact->email());
    body["displayAs"] = contact->name();
    body["fullName"] = contact->name();
    body["firstName"] = firstName;
    body["lastName"] = lastName;
    body["emailAddressList"] = emails;
    if (!orgs.empty()) body["company"] = orgs.front();
    if (!titles.empty()) body["jobTitle"] = titles.front();
    if (!notes.empty()) body["additionalInfo"] = notes.front();
    if (!phones.empty()) {
        json phoneList = json::array();
        for (const string & phone : phones) phoneList.push_back({{"phoneType", "Home"}, {"device", "Unknown"}, {"number", phone}});
        body["phoneNumberList"] = phoneList;
    }
    body["sourceOwner"] = info.value("sourceOwner", account->emailAddress());
    body["sourceId"] = info.value("sourceId", "Contacts");
    if (contact->hidden()) {
        body["contactType"] = 2;
        body["isGroup"] = true;
        vector<json> members;
        vector<json> groupedContacts;
        auto group = store->find<ContactGroup>(Query().equal("id", contact->id()));
        if (group) for (const string & memberId : group->getMembers(store)) {
            auto member = store->find<Contact>(Query().equal("id", memberId));
            if (member && !member->email().empty()) {
                members.push_back({{"emailAddress", member->email()}, {"displayName", member->name()}});
                groupedContacts.push_back({{"type", 0}, {"emailAddress", member->email()},
                    {"displayName", member->name()}, {"addressType", "SMTP"}, {"sendAsMime", true},
                    {"preferredFormat", 0}, {"encs", 0}, {"folderId", 0}, {"contactId", 0}});
            }
        }
        body["members"] = members;
        body["groupedContacts"] = groupedContacts;
        // SmarterMail treats the second contact-put path segment as a folder for
        // groups. Replace a group atomically from the client's perspective.
        SmarterMailClient(account).saveContact("", body);
        if (!remoteId.empty()) SmarterMailClient(account).deleteContact(info.value("sourceOwner", account->emailAddress()), info.value("sourceId", "Contacts"), remoteId);
    } else {
        SmarterMailClient(account).saveContact(remoteId, body);
    }
    runContacts();
    if (created) {
        auto temporary = store->find<Contact>(Query().equal("id", contact->id()));
        if (temporary && temporary->source() == SMARTERMAIL_SYNC_SOURCE && temporary->bookId() != account->id() + BOOK_SUFFIX) {
            auto temporaryGroup = store->find<ContactGroup>(Query().equal("id", temporary->id()));
            if (temporaryGroup) store->remove(temporaryGroup.get());
            store->remove(temporary.get());
        }
    }
}

void SmarterMailDataWorker::deleteContact(shared_ptr<Contact> contact) {
    json info = contact->info();
    string remoteId = info.value("remoteId", "");
    if (remoteId.empty()) throw SyncException("smartermail-contact-no-id", "Cannot delete a SmarterMail contact without its remote identifier.", false);
    SmarterMailClient(account).deleteContact(info.value("sourceOwner", account->emailAddress()), info.value("sourceId", "Contacts"), remoteId);
    store->remove(contact.get());
}

void SmarterMailDataWorker::rebuildContactGroup(shared_ptr<Contact> contact) {
    auto group = store->find<ContactGroup>(Query().equal("id", contact->id()));
    if (!group) group = make_shared<ContactGroup>(contact->id(), account->id());
    group->setName(contact->name());
    group->setBookId(contact->bookId());
    store->save(group.get());
}

void SmarterMailDataWorker::writeAndResyncEvent(shared_ptr<Event> event) {
    auto calendar = store->find<Calendar>(Query().equal("id", event->calendarId()));
    if (!calendar || !calendar->_data.count("smCalendarId")) throw SyncException("no-calendar", "SmarterMail calendar not found for event syncback.", false);
    ICalendar parsed(event->icsData());
    if (parsed.Events.empty()) throw SyncException("invalid-event", "The event data could not be parsed.", false);
    ICalendarEvent * source = parsed.Events.front();
    string timezone = timezoneFromICS(event->icsData());
    SmarterMailClient client(account);
    string apiOwner = calendarOwnerForWrite(client, calendar, account->emailAddress());
    json body = {{"calendarId", calendar->_data["smCalendarId"]}, {"calendarOwner", apiOwner},
                 {"subject", source->Summary}, {"description", source->Description}, {"location", source->Location},
                 {"allDay", !source->DtStart.WithTime}, {"start", dateEnvelope(source->DtStart, timezone)},
                 {"end", dateEnvelope(source->DtEnd.IsEmpty() ? source->DtStart : source->DtEnd, timezone)}};
    string remoteId = event->_data.value("smEventId", "");
    if (event->_data.count("smartermail")) remoteId = eventId(event->_data["smartermail"]);
    if (!remoteId.empty()) body["uid"] = remoteId;
    if (!source->RRule.IsEmpty()) body["rrule"] = source->RRule.operator string();
    // Attendees are intentionally omitted: SmarterMail sends duplicate invites
    // when its API receives attendees on ordinary event updates.
    client.saveCalendarEvent(apiOwner, calendar->_data["smCalendarId"], remoteId, body);
    runCalendars();
}

void SmarterMailDataWorker::deleteEvent(shared_ptr<Event> event) {
    auto calendar = store->find<Calendar>(Query().equal("id", event->calendarId()));
    string remoteId = event->_data.value("smEventId", "");
    if (event->_data.count("smartermail")) remoteId = eventId(event->_data["smartermail"]);
    if (!calendar || remoteId.empty()) throw SyncException("smartermail-calendar-no-id", "Cannot delete a SmarterMail event without its remote identifiers.", false);
    SmarterMailClient client(account);
    string apiOwner = calendarOwnerForWrite(client, calendar, account->emailAddress());
    client.deleteCalendarEvent(apiOwner, calendar->_data.value("smCalendarId", ""), remoteId);
    store->remove(event.get());
}
