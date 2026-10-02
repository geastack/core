// The audio declarations include the Worker bridge even in non-worker apps.
// This translation unit must also compile against the released compiler runtime,
// whose ArrayBuffer is a byte vector without detachment methods.
#include "gea/audio-worklet-runtime.h"

int main() {
  gea::runtime::hostworker::TransferList transfers;
  return transfers.buffers.empty() && transfers.ports.empty() ? 0 : 1;
}
