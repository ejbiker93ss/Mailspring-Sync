#include "../MailSync/FolderSyncPolicy.hpp"

using namespace FolderSyncPolicy;

static_assert(statusIsUsable(42, 1));
static_assert(statusIsUsable(42, 900));
static_assert(!statusIsUsable(0, 900));
static_assert(!statusIsUsable(42, 0));

static_assert(shouldForceShallowScan(false, true, 10, 9));
static_assert(shouldForceShallowScan(false, true, 10, 11));
static_assert(!shouldForceShallowScan(false, true, 10, 10));
static_assert(!shouldForceShallowScan(false, false, 0, 10));
static_assert(!shouldForceShallowScan(true, true, 10, 9));

static_assert(backgroundPollIntervalSeconds(false, false) == 30);
static_assert(backgroundPollIntervalSeconds(false, true) == 120);
static_assert(backgroundPollIntervalSeconds(true, false) == 120);

int main() {
    return 0;
}
