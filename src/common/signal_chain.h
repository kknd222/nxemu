#pragma once

#include <signal.h>

namespace Common {
int SigAction(int signum, const struct sigaction* act, struct sigaction* oldact);
}
