# Reading a V6 physical result

Only observed typing or a command response proves keyboard input. A lit LED,
published HC, successful control request or interrupt packet is a separate
milestone. V5's LED remained on but no keys were observed.

The last failed operation is cached history and survives successful traffic.
Control phase 0 means validation/readiness, 1 SETUP, 2 DATA, 3 STATUS. Read
the request, address, phase and result together with its EP0 trace.

| EP0 cause | Recorded exit from WaitEp0 |
| --- | --- |
| 0 / none | No failing WaitEp0 was reached; inspect the other failure fields |
| 1 / role lost | CheckRole failed; hardware may already be inaccessible |
| 2 / CSR error | CSR0_ERROR was observed |
| 3 / host or VBUS lost | DEVCTL lacked host mode or a valid VBUS indication |
| 4 / stall | CSR0_RXSTALL was observed |
| 5 / NAK | CSR0_NAKTIMEOUT was observed |
| 6 / timeout | The polling budget expired |

These are software branch observations, not proof of a defective hub, PHY
or electrical cause. ValidMask bits 1/2/4 identify CSR0/INTRTX/DEVCTL values
from one polling iteration. Dashes mean unavailable, not zero. On role loss,
HardwareAccessLost marks a retained sample from before that failed role check;
no registers were read after the hardware became inaccessible. DEVCTL may
be unavailable on CSR error because that iteration never reached its read.

Polls counts loop iterations; DelayUs counts explicit 10-us delays in that
WaitEp0 call. Neither is electrical timing or full request latency. The
root-reset DEVCTL/POWER sample is separate history captured after deassertion
of a completed root reset, invalidated when a new actual root reset starts.

Inventory counts interfaces, not unique physical devices. Read locate/errors
before interpreting zeros. Hub -> HID boot keyboard -> TextIn -> routed
console input are successive observations, each requiring its own evidence.
Last IN metadata contains length and callback status, not keystroke payload.

Diagnostics may move the displayed Shell prompt while its reader is still
waiting. The observer does not read keys or fabricate Shell>. Detached DRP
changes are throttled even with a published HC; actual errors and device or
inventory changes can still print a block.

For the physical test, connect hub plus keyboard in the single dispatched
boot, wait 30-60 seconds, try help followed by Enter, and photograph the last
failure/EP0/root-reset/inventory lines if there is no response. Do not infer
success from the absence of new diagnostics or from the LED alone.
