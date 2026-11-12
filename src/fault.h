#pragma once

// Fault-injection points for crash testing.
//
// The engine calls trigger() at the three moments where a crash matters most
// in commit(). Normally no hook is installed and trigger() is a single atomic
// load. Tests install a hook that kills the process (std::_Exit) or throws
// an IoError at a chosen point, so we can check what recovery sees when the
// process dies exactly there.

#include <atomic>

namespace forgedb::fault {

enum class Point {
    kBeforeWrite,             // transaction encoded, nothing written yet
    kAfterWriteBeforeSync,    // bytes handed to the OS, not yet fsynced
    kAfterSyncBeforePublish,  // durable on disk, not yet visible in the index
};

using Hook = void (*)(Point);

void set_hook(Hook hook);  // nullptr removes the hook
void trigger(Point point);

}  // namespace forgedb::fault
