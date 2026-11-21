// Bridging header for the shipped target.
//
// The shipped build deliberately compiles and links no iSH code, so this header
// exposes only the runtime execution probe, which is original code with no GPL
// dependency. Keeping it separate from TierCProbe-Bridging-Header.h means the
// exclusion is structural rather than a matter of not calling into the backend.

#import "RuntimeExecProbe.h"

// Tier D: the E2 harness bridge.
#include "E2Bridge.h"
