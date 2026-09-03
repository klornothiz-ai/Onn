// ProsperoLayer PS5 emulator - controller (Pad) real implementation.
// NOTE: This file is intentionally excluded from the default build
// (controller.cpp is the canonical implementation). It exists as a
// non-conflicting shim that mirrors the Kyty controller surface for
// tooling that expects a TU named controller_stub.
//
// All Pad functions are implemented in libs/controller.cpp and exported
// through the symbol database (Libs::InitPad_1). See that file.
//
// This TU intentionally contains no definitions; linking it is harmless.
