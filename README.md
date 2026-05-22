# X3270 — Free TN3270 Terminal Emulator for macOS

A native macOS (ARM - Apple Silicon + Intel) TN3270/TN3270E terminal emulator for connecting to IBM Mainframes (z/OS, z/VM, z/VSE).  
Built entirely in C++ and Objective-C++ on top of native Cocoa, CoreText and OpenSSL.  
**No license fee. No Java. No X11.**

---

## Why this exists

If you work with IBM Mainframes on a Mac, you've probably noticed that every halfway-decent TN3270 terminal client costs money — sometimes a *lot* of money. We're talking $50–$100+ for software that essentially emulates a 1970s text terminal. One popular commercial option charges a recurring subscription just to type on a green screen. That's absurd.

There are a handful of free alternatives, but they either require Java (slow, ugly, a security nightmare), run inside X11 (no thanks), or are abandonware that hasn't been touched in a decade and breaks on every macOS release.

So I built one from scratch. Native Cocoa. Native CoreText rendering. OpenSSL for TLS. Full TN3270E negotiation including ISPF Query Reply so the menus actually appear. It took a weekend of frustration and a lot of reading ancient IBM manuals — but the result is a clean, fast, free terminal that feels like it belongs on a Mac.

If you work in Mainframe and you're tired of paying for the privilege, this is for you.

---

## Screenshot

![ISPF Primary Option Menu running inside X3270](screenshots/screenshot.png)

*ISPF 8.1 Primary Option Menu on z/OS — connected to IBM ZExplore mainframe at 204.90.115.200:623*

---

## Features

| Feature | Details |
|---|---|
| **Protocol** | TN3270E (RFC 2355) with automatic fallback to classic TN3270 |
| **Security** | Plain Telnet **and** implicit TLS (TLS 1.2+) on any port |
| **Screen models** | IBM 3278 Model 2 (24 × 80) |
| **EBCDIC code pages** | CP037 (US), CP500 (International), CP1047 (Open Systems) |
| **UI** | Native Cocoa window, green-on-black phosphor, 600 ms cursor blink |
| **Keyboard** | PF1–PF24, PA1–PA3, Clear, Reset, Tab/BackTab, ErEOF, Insert, arrows |
| **Query Reply** | Responds to IBM Structured Field Read Partition Query (required for ISPF) |
| **Rendering** | CoreText glyph metrics for pixel-perfect character grid |
| **macOS** | 12 Monterey and later (Apple Silicon + Intel) |

---

## Download

Pre-built DMG releases are available on the [**Releases**](https://github.com/el-dockerr/X3270/releases) page.
Every push to `main` automatically builds and publishes a new DMG via GitHub Actions.

1. Download `X3270-<version>.dmg`
2. Open the DMG and drag **X3270.app** to your `/Applications` folder
3. On first launch: right-click → **Open** (macOS Gatekeeper; the app is unsigned)

---

## Connecting to a Mainframe

1. Launch X3270 — the **Connect** dialog opens automatically
2. Fill in:
   | Field | Example |
   |---|---|
   | Host | `204.90.115.200` |
   | Port | `623` (plain) · `992` (TLS) · `23` (standard Telnet) |
   | SSL/TLS | check for encrypted connections |
   | CA Bundle | path to a PEM file if using a private CA (optional) |
   | Code Page | CP037 (US default) · CP500 · CP1047 |
3. Click **Connect**

The terminal window opens. Type your credentials at the logon screen. ISPF and TSO sessions are fully supported.

---

## Keyboard Map

| Key | 3270 Function |
|---|---|
| `F1`–`F12` | PF1–PF12 |
| `Shift`+`F1`–`F12` | PF13–PF24 |
| `Option`+`1`/`2`/`3` | PA1 / PA2 / PA3 |
| `Return` | Enter (AID) |
| `Escape` | Reset (unlock keyboard) |
| `Option`+`Escape` | Clear screen |
| `Tab` / `Shift`+`Tab` | Next / previous field |
| `Insert` | Toggle insert mode |
| `Option`+`Delete` | Erase to End of Field |
| `Option`+`E` | Erase Input (all unprotected fields) |
| `↑` `↓` `←` `→` | Cursor movement |

---

## Building from Source

### Prerequisites

```bash
# Xcode Command Line Tools
xcode-select --install

# Homebrew + OpenSSL
brew install openssl@3 cmake
```

### Build

```bash
git clone https://github.com/el-dockerr/X3270.git
cd X3270
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/X3270.app
```

To set an explicit build number (useful in CI):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_NUMBER=42
cmake --build build
```

### Package a DMG for distribution

```bash
./package.sh
# produces: dist/X3270-1.0.0-build1.dmg
```

Pass an optional build number:

```bash
BUILD_NUMBER=42 ./package.sh
# produces: dist/X3270-1.0.0-build42.dmg
```

---

## Version History

### v1.1.0 — 2026-05-22

**IBM 3279 color rendering**
- **Extended attribute support (SA / SFE)** — `Set Attribute` (0x28) and `Set Field Extended` (0x29) structured fields are now fully parsed. Foreground color (type 0x42), background color (type 0x45), and highlighting (type 0x41) attributes are stored per cell and carried through `startField` / `writeChar`.
- **IBM 3279 colour palette** — Seven standard IBM colors rendered correctly: blue (0xF1), red (0xF2), pink (0xF3), green (0xF4), turquoise (0xF5), yellow (0xF6), white (0xF7). Default field color derived from the field attribute Protected/Numeric/MDT bits (green / red / blue / white quadrant).
- **Intensified fields** — Unprotected-intensified fields now render in red (IBM default) instead of white.
- **Reverse video (highlight 0xF2)** — Foreground and background swapped at render time.
- **Underscore (highlight 0xF4)** — 1 px bottom stroke drawn per cell.
- **Per-cell background fill** — Non-default cell backgrounds are filled before drawing the character.
- **Fixed `FA_DISP_LP` constant** — Was `0x08`, corrected to `0x04` per IBM GA23-0059.

**Keyboard / function key fixes**
- **Fixed PF10 / PF11 / PF12 AID codes** — The emulator was sending `0xFA / 0xFB / 0xFC` for these keys. The IBM GA23-0059 standard mandates `0x7A / 0x7B / 0x7C`. ISPF (and all other 3270 hosts) do not recognise the wrong codes, so F12 (and F10/F11) had no effect. Fixed by replacing the broken `0xF0 + n` arithmetic with the correct IBM lookup table. PF22–PF24 (Shift+F10/11/12) were similarly wrong (`0xCA–CC` → `0x4A–4C`).
- **Added `performKeyEquivalent:` override in `TerminalView`** — macOS routes some function-key events through the key-equivalent path (menu shortcut resolution) rather than `keyDown:`, silently dropping them. The override mirrors the full PF1–PF24 / Shift+F1–F12 mapping so those events are consumed by the terminal regardless of which path the OS uses.

**Cursor and OIA improvements**
- **Block cursor** — Replaced the thin underline cursor with a full block cursor (cell filled with cursor color, character re-drawn in background color). The cursor is now visible at all times including when the keyboard is locked.
- **OIA layout** — Version string moved to the lower OIA row to avoid overlapping the status and cursor-position indicators.

**Connect dialog — connection history**
- The **Host** field is now an editable drop-down combo box. Every successful connection is saved to a history list (up to 20 entries, most recent first, deduplicated by host:port).
- Selecting a previous entry from the list automatically restores the paired **Port**, **SSL/TLS**, **CA Bundle**, and **Code Page** settings — no need to re-enter them.
- History is persisted in `NSUserDefaults` across app launches.

### v1.0.3 — 2026-05-22

**ISPF / 3270 data stream fixes**
- **Fixed: ISPF screen input error code 23 on protected fields** — `Read Modified` responses were including protected fields that had MDT=1 (host-written output fields). Per IBM GA23-0059, `Read Modified` must return *only* unprotected (input) fields; sending protected field data back caused ISPF to reject the input with error code 23. Fix: `getModifiedFields()` now skips any FA cell with the Protected bit set.
- **Fixed: `Read Modified All` now correctly returns all MDT fields** — `CMD_READ_MODIFIED_ALL` (0x0E/0x6E) was handled identically to `CMD_READ_MODIFIED`, so the protected-field filter was incorrectly applied to host-solicited "all fields" polls as well. Per spec, `Read Modified All` must include both protected and unprotected modified fields. The two commands are now handled separately; `buildReadModifiedRecord` accepts an `includeProtected` flag.

### v1.0.2 — 2026-05-19

**Traffic Monitor panel**
- New floating **Traffic Monitor** window (Debug → Traffic Monitor, `⌘⇧D`) showing all raw inbound and outbound Telnet/TN3270 bytes as a colour-coded hex dump (TX blue, RX green) with timestamps, byte counts, and a printable ASCII column.
- **Clear** button wipes the log; **Save to File…** exports the full session as plain text.
- Captures traffic from the moment a connection is initiated so the full negotiation is always visible.

**TN3270 / z/VM protocol fixes**
- **Fixed: z/VM stuck at NVT "PRESS BREAK KEY TO BEGIN SESSION"** — The client was proactively sending `WILL BINARY`, `DO BINARY`, `WILL EOR`, `DO EOR` during the opening handshake. z/VM responds with `DONT BINARY` / `DONT EOR`, which per RFC 854 permanently disables those options for the session. z/VM then committed to NVT mode and never offered 3270 negotiation. Fix: remove proactive BINARY/EOR offers from `connect()`; let the server drive binary/EOR negotiation after the terminal-type exchange.
- **Fixed: Duplicate `WILL TN3270E` confusing TN3270E-capable servers** — When the server confirmed our initial `WILL TN3270E` by echoing `DO TN3270E`, the response handler was sending a second `WILL TN3270E`, causing some servers to reject TN3270E entirely. Fix: added `sentWillTN3270E_` / `sentDoTN3270E_` guards (same pattern as the existing `sentWillBinary_` guards).

**Other fixes**
- `WILL TN3270E` server offer not handled → fixed
- `enterDataMode()` guard was too strict (required `willBinary_`/`willEOR_`) → fixed
- Write command reset buffer address to 0 → fixed
- `FUNCTIONS REJECT` from server not handled → fixed
- Keyboard locked permanently after a failed AID send (`SendRecordCallback` now returns `bool`) → fixed
- Keyboard started unlocked instead of locked-while-connecting → fixed (`LockReason::Connecting` initial state)
- `DEVICE-TYPE REJECT` did not call `enterDataMode()` → fixed
- Duplicate `WILL`/`DO` for `BINARY`/`EOR` during re-negotiation → fixed (`sentXxx_` flags)
- `TERMINAL-TYPE SEND` sub-negotiation did not set `doTermType_` → fixed
- Query Reply was missing Colour and Highlighting structured fields (required for ISPF menus) → fixed

### v1.0.1 — 2026-05-13

**Initial public release** — basic TN3270E support, TLS support, ISPF Query Reply support, CoreText rendering, keyboard input, and a simple Connect dialog.


---

## License

X3270 for macOS is released under the **MIT License**.  
See [LICENSE](LICENSE) for the full text.

Written by Swen Kalski, 2026.

IBM, z/OS, ISPF, and 3270 are trademarks of IBM Corporation.  
This project is not affiliated with or endorsed by IBM.
