#include "../MailSync/CardDAVDiscoveryPolicy.hpp"

#include <cassert>

int main() {
    using CardDAVDiscoveryPolicy::addressBookPreference;

    assert(addressBookPreference("My Contacts", "/WebDAV/ab/user/") >
           addressBookPreference("Global Address List", "/WebDAV/gal/"));
    assert(addressBookPreference("Personal", "/addressbooks/me/default/") >
           addressBookPreference("Directory", "/addressbooks/domain/"));
    assert(addressBookPreference("Contacts", "/contacts/") > 0);
}
