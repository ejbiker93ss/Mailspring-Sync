#ifndef FolderSyncPolicy_hpp
#define FolderSyncPolicy_hpp

#include <cstdint>

namespace FolderSyncPolicy {

constexpr bool statusIsUsable(uint32_t uidValidity, uint32_t uidNext) {
    // RFC 3501 defines UIDVALIDITY as non-zero and UIDNEXT as the next usable
    // UID, so even an empty mailbox reports at least 1 for UIDNEXT.
    return uidValidity > 0 && uidNext > 0;
}

constexpr bool shouldForceShallowScan(
    bool hasQResync,
    bool hasSavedMessageCount,
    uint32_t savedMessageCount,
    uint32_t remoteMessageCount)
{
    return !hasQResync && hasSavedMessageCount && savedMessageCount != remoteMessageCount;
}

constexpr int backgroundPollIntervalSeconds(bool usesMicrosoftGraph, bool supportsIdle) {
    // IDLE accounts have a foreground push loop. Plain IMAP accounts need a
    // shorter fallback poll so changes made by another client do not look stuck.
    return !usesMicrosoftGraph && !supportsIdle ? 30 : 120;
}

} // namespace FolderSyncPolicy

#endif /* FolderSyncPolicy_hpp */
