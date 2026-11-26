#pragma once
#include "caw_bus.h"

// The LIVE Pure Data mix for the VST rack's "PureData" inputs (engine 1). The dedicated PD
// engine (pd_daw_engine.h) pushes its latest rendered block here from its own audio thread;
// the rack pulls it per block - the same ring handoff the sample / SuperCollider tracks use.
// Mixed PD mode keeps the runner's own pdMix pointer (this ring stays empty there), and the
// offline C++ render prepares its own mixer.
namespace pdlivering {
inline CawScRing ring;
}
