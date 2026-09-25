#include "../MailSync/MoveResult.hpp"

#include <cassert>

int main() {
    using nlohmann::json;

    assert(MoveResult::mapped(json{{"destinationUIDs", {{"1", "101"}}}}, 1) == 101);
    assert(MoveResult::mapped(json::array({{{"uid", 2}, {"newUid", 102}}}), 2) == 102);
    assert(MoveResult::mapped(json{{"destinationUIDs", {101, 102}}}, 1) == 0);

    const std::vector<json> rows = {
        {{"uid", 101}, {"messageId", "<test@example.invalid>"}}
    };
    assert(MoveResult::newlyObserved(rows, {}, "test@example.invalid") == 101);
    assert(MoveResult::newlyObserved(rows, {101}, "test@example.invalid") == 0);
    assert(MoveResult::newlyObserved(rows, {}, "different@example.invalid") == 0);

    assert(MoveResult::sourceMayStillBeStale(100, 219));
    assert(!MoveResult::sourceMayStillBeStale(100, 220));
    assert(!MoveResult::sourceMayStillBeStale(0, 100));
}
