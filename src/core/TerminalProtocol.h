#pragma once

namespace x3270 {

/// High-level protocol selection — determines session type and data stream parser.
enum class TerminalProtocol {
    TN3270,  ///< IBM 3270 (z/OS, z/VM, z/VSE) — uses TN3270E when available
    TN5250,  ///< IBM 5250 (AS/400 / IBM i)    — standard TN5250 per RFC 2877
};

} // namespace x3270
