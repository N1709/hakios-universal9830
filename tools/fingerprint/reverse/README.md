# Local FINGE decompiler audit

Use the existing locally installed Ghidra/JDK and the imported `finge` project.
Do not commit the imported firmware, generated C, private template, calibration,
HAT, secure-storage or credentials. This directory distributes only analysis tooling.

Ghidra can misinterpret AArch64 compiler-outlined arithmetic-and-branch stubs as
calls to separate functions. Check the instruction listing before trusting a
short decompile that stops at `FUN_0010fb..`.

`DecompileOutlined.java` temporarily joins a specified function range and only
those two-instruction outlines in `0x10f000..0x10ffff` whose second instruction
branches back into the range. The range must be verified manually from the
prologue, epilogue and control flow. This heuristic is specific to this TA;
it is not a general function-boundary detector. Always use `-readOnly` so the
original Ghidra project is preserved. It never patches or resigns firmware.

From the repository root, adjusting the local analysis-tool directory:

```sh
RE_HOME=<workspace>/tools/re
export JAVA_HOME="$RE_HOME/jdk-25.0.5+8/Contents/Home"
export RE_OUT="$RE_HOME/audit-out"
mkdir -p "$RE_OUT"
"$RE_HOME/ghidra_12.2_PUBLIC/support/analyzeHeadless" "$RE_HOME/proj" finge \
  -process finge.elf -readOnly -noanalysis \
  -scriptPath tools/fingerprint/reverse -postScript DecompileOutlined.java \
  15341c:153620 11f750:11fbe0
```

Addresses are Ghidra virtual addresses with image base +0x100000; the end is
exclusive. Both functions were reconstructed successfully with Ghidra 12.2.
Outputs stay in `RE_OUT`. Read the protocol audit in `docs/devices/x1slte/fingerprint.md`
for the actual wire layout and the limits of the current evidence.
