#include "../MailSync/CalendarSyncPolicy.hpp"
#include <cassert>

int main() {
    using namespace CalendarSyncPolicy;
    const long long now = 1788880000;
    assert(reconciliationDue(0, now));
    assert(!reconciliationDue(now, now));
    assert(!reconciliationDue(now - 3599, now));
    assert(reconciliationDue(now - 3600, now));
    assert(reconciliationDue(now + 60, now)); // clock rollback must not freeze sync
    assert(!reconciliationDue(now - 2700, now)); // ordinary 45 minute poll stays cheap
    assert(reconciliationDue(now - 5400, now));
    assert(hrefText("/WebDAV/cal/a%2Fb%2540c+name.ics") == "/WebDAV/cal/a%2Fb%2540c+name.ics");
    assert(hrefText("/a&b<c>.ics") == "/a&amp;b&lt;c&gt;.ics");
    assert(hrefText("https://mail.example.com/a%20b.ics") == "https://mail.example.com/a%20b.ics");
}
