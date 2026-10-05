.section .mmu3c2h.telemetry.dispatcher, "ax"
.align 2

.global MotoG20TelemetryV2PublishByMState
.type MotoG20TelemetryV2PublishByMState, %function
.extern MotoG20TelemetryV2PublishPreMmu
.extern MotoG20TelemetryV2PublishPostMmu

// AAPCS64 tail dispatcher. X0/X1 remain the original publisher arguments.
// The sole predicate is the captured SCTLR_EL2.M bit at ABI offset 0x60.
MotoG20TelemetryV2PublishByMState:
    ldr     x2, [x0, #0x60]
    tbz     x2, #0, MotoG20TelemetryV2PublishPreMmu
    b       MotoG20TelemetryV2PublishPostMmu

.size MotoG20TelemetryV2PublishByMState, .-MotoG20TelemetryV2PublishByMState
