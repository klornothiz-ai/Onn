// ProsperoLayer PS5 emulator - dialog kernel real implementation.
// NOTE: This file is intentionally excluded from the default build
// (dialog.cpp is the canonical implementation with ~50 dialog functions
// covering CommonDialog / MsgDialog / ImeDialog). It exists as a
// non-conflicting shim for tooling that expects a TU named
// dialog_kernel_stub.
//
// This TU intentionally contains no definitions; linking it is harmless.
