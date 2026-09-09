#ifndef CardDAVDiscoveryPolicy_hpp
#define CardDAVDiscoveryPolicy_hpp

#include <algorithm>
#include <cctype>
#include <string>

namespace CardDAVDiscoveryPolicy {

inline int addressBookPreference(const std::string & displayName, const std::string & url) {
    std::string searchable = displayName + " " + url;
    std::transform(
        searchable.begin(),
        searchable.end(),
        searchable.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );

    int preference = 0;
    if (searchable.find("my contacts") != std::string::npos) preference += 100;
    if (searchable.find("personal") != std::string::npos) preference += 80;
    if (searchable.find("contacts") != std::string::npos) preference += 20;
    if (searchable.find("global") != std::string::npos) preference -= 100;
    if (searchable.find("address list") != std::string::npos) preference -= 100;
    if (searchable.find("directory") != std::string::npos) preference -= 100;
    if (searchable.find("/gal") != std::string::npos) preference -= 100;
    return preference;
}

} // namespace CardDAVDiscoveryPolicy

#endif /* CardDAVDiscoveryPolicy_hpp */
