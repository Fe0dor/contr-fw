# Consolidation 0.6.4

Source: verified snapshot contr-fw-0.6.4-20261023-122248, base 27757a44dec8ab97fd949e031d9bc77830caa5db.
Archive SHA256: 3983bf585bb1639f7e65802d48ce36f239f35965b0068272354c8d543c3aee93.
All tracked and untracked source/test/document files restored. Ignored build, caches, logs,
private external outputs and hardware tooling excluded from publication. Historical reports
already under docs/protocols are retained as evidence, not new acceptance.
Submodules restored from archived .git/modules (shallow bundles alone are insufficient).
No upstream dependency patches intended; all six working trees must be clean.

Firmware 0.6.4; UI/emulator package versions remain 0.6.1. No hardware actions in consolidation.
QA U43 remains unresolved: isolated output measured 0.37–0.46 V, adjacent channels work;
damage is suspected, not proven. SPI FIFO wait fix did not resolve this symptom.
SR0 loop disabled, SR1 unverified; section power/measurements, physical relay contacts,
MISO PE5, hardware update/rollback acceptance, RSP mains-loss and HLG measured acceptance
remain open. RSP history dependence persists; calibration does not guarantee ±0.10 V.
Browser DOM model tests do not replace visual Chrome acceptance.

Local acceptance: generator check; fresh host build and CTest; pytest; Node PSU panel;
host/emulator dialogs; relay contract; target and bringup builds. HIL flags remain OFF.
