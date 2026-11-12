#include "fault.h"

namespace forgedb::fault {
namespace {
std::atomic<Hook> g_hook{nullptr};
}

void set_hook(Hook hook) { g_hook.store(hook); }

void trigger(Point point) {
    if (Hook hook = g_hook.load()) hook(point);
}

}  // namespace forgedb::fault
