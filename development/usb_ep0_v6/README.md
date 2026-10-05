# USB V6: EP0 failure evidence and detached-console throttling

V5 E1E431DA is consumed and physically observed. The user reports that the
hub LED stayed on, but no keyboard input appeared. Its photograph shows one
failed SET_ADDRESS(1) request at address 0, phase SETUP, result 0x100 SYSTEM.
No UsbIo interfaces, HID interfaces or console inputs were discovered. The
photo does not distinguish role loss, CSR0_ERROR or loss of host/VBUS status.
Later detached status is not evidence of the electrical state at the failure.

V6 records the specific WaitEp0 exit cause and the register values already
read by that polling iteration, before transfer cleanup. A failed role check
never triggers additional HC reads; any earlier sample is explicitly marked
historical. Poll and delay counters are diagnostic, not measured wall time.
A separate cached DEVCTL/POWER sample records the completed root-port reset.
Transfer algorithms, PHY configuration and timing are unchanged.

The console observer now normalizes detached role changes even if the HC
protocol remains published. This fixes the V5 repeated-block case after
unplugging. Errors and actual connection/inventory changes remain observable.
It does not print a synthetic Shell prompt or consume keyboard input.

The user-requested unlimited detach-qualified session policy remains:
SessionLimit=0, stable detach 500 ms, qualification deadline 1100 ms, and
fatal shutdown handling. There is no battery-charge minimum. A continuously
attached failed device does not trigger repeated power pulses.

Model tests and AARCH64 compilation do not prove physical keyboard input.
This candidate requires a new identity and its own single dispatch. V5 is
not replayed. See DIAGNOSTIC_GUIDE.md for interpreting the next photograph.
