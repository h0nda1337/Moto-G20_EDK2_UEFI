// Recompile the frozen MMU3C2C bridge in isolation with its sole publisher
// call redirected to the exact 12-byte M-state dispatcher. The bridge body,
// frame geometry, register capture, and stack frame remain source-identical.
#define MotoG20TelemetryV2PublishPreMmu MotoG20TelemetryV2PublishByMState
#include "../MMU3C2C/MotoG20Mmu3c2cTelemetry.c"
