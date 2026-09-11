#include "DavXML.hpp"
#include "SyncException.hpp"
#include "CalendarSyncPolicy.hpp"
#include "icalendar.h"
#include <cassert>
#include <iostream>
int main() {
    const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:xml-invalid@test\r\nDTSTART:20260911T150000Z\r\nDTEND:20260911T160000Z\r\nSUMMARY:Recovered meeting\r\nDESCRIPTION:before\x0b" "after\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    bool caught = false;
    try {
        DavXML invalid("<d:multistatus xmlns:d=\"DAV:\"><d:response>" + ics + "</d:response></d:multistatus>", "https://example.test/calendar");
    } catch (SyncException& error) {
        caught = true;
        assert(error.key == "invalid-dav-xml");
        assert(error.debuginfo.find("Recovered meeting") == std::string::npos);
    }
    assert(caught);
    ICalendar raw(ics);
    assert(raw.Events.size() == 1);
    assert(raw.Events.front()->Summary == "Recovered meeting");
    assert(!raw.Events.front()->DtStart.IsEmpty());
    DavXML valid("<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/event.ics</d:href></d:response></d:multistatus>", "https://example.test/calendar");
    assert(valid.nodeContentAtXPath("//D:href/text()") == "/event.ics");
    using CalendarSyncPolicy::resourcePath;
    assert(resourcePath("https://other.test/cal/a%2Fb+name.ics") == "/cal/a%2Fb+name.ics");
    assert(resourcePath("/cal/a%2540b.ics") == "/cal/a%2540b.ics");
    assert(resourcePath("event.ics") == "event.ics");
    assert(resourcePath("https://example.test").empty());
    std::cout << "PASS: malformed XML is catchable, diagnostics omit event data, raw ICS remains intact, normal XML and resource paths work" << std::endl;
}
