#include "DataStream5250Parser.h"
#include <algorithm>

namespace x3270 {

DataStream5250Parser::DataStream5250Parser(ScreenBuffer& screen)
    : screen_(screen) {}

// ── Public entry point ────────────────────────────────────────────────────────

void DataStream5250Parser::processRecord(const std::vector<uint8_t>& record) {
    if (record.empty()) return;

    const uint8_t* data = record.data();
    size_t         len  = record.size();

    // Reset FSM for each new record; may be adjusted by opcode below.
    state_ = ParseState::Command;
    sohRemaining_ = 0;
    tdRemaining_  = 0;

    // Detect and strip GDS header (6 bytes: len_hi, len_lo, 0x12, 0x00, opcode, 0x00).
    // IMPORTANT: the opcode is *inside* the header, not in the payload.
    // After stripping we must set the FSM state based on the opcode, not leave it
    // at Command (which would misinterpret WCC1 as a command byte).
    if (len >= 6) {
        uint16_t encoded = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        if (data[2] == 0x12 && encoded == static_cast<uint16_t>(len)) {
            uint8_t opcode = data[4];
            data += 6;
            len  -= 6;

            switch (opcode) {
            case CMD5250_CLEAR_UNIT:
            case CMD5250_CLEAR_UNIT_ALT:
                screen_.eraseAll();
                if (unlockCb_) unlockCb_();
                return;   // no payload
            case CMD5250_WRITE_ERROR_CODE:
                return;   // ignore
            case CMD5250_WTD:
                // Payload begins with WCC1 + WCC2, not a command byte.
                if (len == 0) return;
                state_ = ParseState::WCC1;
                break;
            default:
                if (len == 0) return;
                break;    // unknown opcode with payload: try as command stream
            }
        }
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        switch (state_) {
        case ParseState::Command:
            handleCommand(b);
            break;
        case ParseState::WCC1:
            // WCC1: bit 7 = lock keyboard, bit 2 = reset MDT
            state_ = ParseState::WCC2;
            break;
        case ParseState::WCC2:
            if (b & 0x80) {
                // Alarm bit in WCC2 bit 7 (some implementations)
                if (alarmCb_) alarmCb_();
            }
            // After WCC bytes, signal unlock and enter data stream
            if (unlockCb_) unlockCb_();
            state_ = ParseState::Data;
            break;
        case ParseState::Data:
            handleDataByte(b);
            break;

        case ParseState::SOH_Length:
            sohRemaining_ = b;  // number of bytes to skip
            state_ = (b > 0) ? ParseState::SOH_Data : ParseState::Data;
            break;
        case ParseState::SOH_Data:
            if (--sohRemaining_ == 0) state_ = ParseState::Data;
            break;

        case ParseState::TD_LenHi:
            tdRemaining_ = static_cast<uint16_t>(b) << 8;
            state_ = ParseState::TD_LenLo;
            break;
        case ParseState::TD_LenLo:
            tdRemaining_ |= b;
            state_ = (tdRemaining_ > 0) ? ParseState::TD_Data : ParseState::Data;
            break;
        case ParseState::TD_Data:
            screen_.writeChar(b);
            if (--tdRemaining_ == 0) state_ = ParseState::Data;
            break;

        case ParseState::SBA_Row: {
            uint8_t row = b;
            // Peek at next byte for col
            if (i + 1 < len) {
                uint8_t col = data[++i];
                int offset = rowColToOffset(row, col);
                if (offset >= 0) screen_.setBufferAddress(offset);
            }
            state_ = ParseState::Data;
            break;
        }

        case ParseState::SF_FFW1:
            currentFFW1_ = b;
            state_ = ParseState::SF_FFW2;
            break;
        case ParseState::SF_FFW2:
            currentFFW2_ = b;
            // Check for FCW pairs: FFW2 bit 0 indicates one FCW follows
            // For MVP, parse any trailing FCW pairs (each 2 bytes)
            // FCW presence flags: FFW2 & 0x01 → one FCW pair; we skip them.
            {
                uint8_t attr = ffw1ToAttr(currentFFW1_);
                screen_.startField(attr, 0x00, 0x00, 0x00);
                // If MDT was set in FFW1, mark the field
                if (currentFFW1_ & FFW1_MDT) {
                    screen_.at(screen_.bufferPointer() > 0
                                ? screen_.bufferPointer() - 1 : 0).attr |= 0x01; // FA_MDT
                }
            }
            // FCW skipping: bit 0 of FFW2 signals one FCW pair (2 bytes)
            if (currentFFW2_ & 0x01) {
                state_ = ParseState::SF_FCW_Hi;
            } else {
                state_ = ParseState::Data;
            }
            break;
        case ParseState::SF_FCW_Hi:
            state_ = ParseState::SF_FCW_Lo;
            break;
        case ParseState::SF_FCW_Lo:
            state_ = ParseState::Data;
            break;

        case ParseState::ATTR_Byte:
            // 5250 attribute byte: colour nibble in bits 5-2
            // Map to IBM 3279 fg colour
            currentFgColor_ = mapColor((b >> 2) & 0x07);
            screen_.setCurrentFgColor(currentFgColor_);
            state_ = ParseState::Data;
            break;

        case ParseState::RA_Row:
            raRow_ = b;
            state_ = ParseState::RA_Col;
            break;
        case ParseState::RA_Col:
            raCol_ = b;
            state_ = ParseState::RA_Char;
            break;
        case ParseState::RA_Char: {
            int dest = rowColToOffset(raRow_, raCol_);
            if (dest >= 0) screen_.repeatToAddress(dest, b);
            state_ = ParseState::Data;
            break;
        }

        case ParseState::EA_Row:
            raRow_ = b;
            state_ = ParseState::EA_Col;
            break;
        case ParseState::EA_Col: {
            int dest = rowColToOffset(raRow_, b);
            if (dest >= 0) screen_.eraseUnprotectedToAddress(dest);
            state_ = ParseState::Data;
            break;
        }
        }
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────────

void DataStream5250Parser::handleCommand(uint8_t cmd) {
    switch (cmd) {
    case CMD5250_WTD:
        state_ = ParseState::WCC1;
        break;

    case CMD5250_CLEAR_UNIT:
    case CMD5250_CLEAR_UNIT_ALT:
        screen_.eraseAll();
        if (unlockCb_) unlockCb_();
        state_ = ParseState::Command; // no further data expected
        break;

    case CMD5250_WRITE_ERROR_CODE:
        // Phase 1: ignore — host is sending an error indicator
        // (typically followed by a subsequent WTD to redraw the screen)
        state_ = ParseState::Command;
        break;

    case CMD5250_SAVE_SCREEN:
    case CMD5250_RESTORE_SCREEN:
        // Phase 1: not supported — treat as no-op
        state_ = ParseState::Command;
        break;

    default:
        // Unknown command: skip
        state_ = ParseState::Command;
        break;
    }
}

// ── Order dispatch inside WTD data stream ────────────────────────────────────

void DataStream5250Parser::handleDataByte(uint8_t b) {
    // In the 5250 data stream, bytes below 0x20 that match order codes are orders.
    // Displayable EBCDIC starts at 0x40.  Bytes 0x20-0x3F are orders (some).
    // Bytes 0x00-0x1F that don't match orders are written as spaces.
    switch (b) {
    case ORD5250_SOH:
        state_ = ParseState::SOH_Length;
        break;
    case ORD5250_TD:
        state_ = ParseState::TD_LenHi;
        break;
    case ORD5250_SBA:
        state_ = ParseState::SBA_Row;
        break;
    case ORD5250_SF:
        state_ = ParseState::SF_FFW1;
        break;
    case ORD5250_ATTR:
        state_ = ParseState::ATTR_Byte;
        break;
    case ORD5250_IC:
        screen_.insertCursorHere();
        break;
    case ORD5250_RA:
        state_ = ParseState::RA_Row;
        break;
    case ORD5250_EA:
        state_ = ParseState::EA_Row;
        break;
    default:
        if (b >= 0x40) {
            // Normal EBCDIC display character
            screen_.writeChar(b);
        }
        // else: unrecognised control byte — skip silently
        break;
    }
}

// ── Attribute mapping ─────────────────────────────────────────────────────────

uint8_t DataStream5250Parser::ffw1ToAttr(uint8_t ffw1) const {
    uint8_t attr = 0x00;

    // Protected / bypass field
    if (ffw1 & FFW1_BYPASS) {
        attr |= FA_PROTECTED | FA_NUMERIC; // skip (same as 3270 skip)
    } else if (ffw1 & FFW1_SHIFT_NUM) {
        attr |= FA_NUMERIC;
    }

    // Display mode
    uint8_t disp = ffw1 & FFW1_DISP_MASK;
    if (disp == FFW1_DISP_INTEN) {
        attr |= FA_INTENSIFIED;
    } else if (disp == FFW1_DISP_NONE || disp == FFW1_DISP_SIGN) {
        attr |= FA_NONDISPLAY;
    }

    // MDT
    if (ffw1 & FFW1_MDT) {
        attr |= FA_MDT;
    }

    return attr;
}

uint8_t DataStream5250Parser::mapColor(uint8_t c3) const {
    // 5250 colour nibble (3 bits) → IBM 3279 palette code
    // AS/400 colour table per IBM 5250 Data Stream Programmer's Reference:
    // 0=Green, 1=Green/Reverse, 2=White, 3=White/Reverse,
    // 4=Green/Underline, 5=Green/Reverse/Underline, 6=NonDisplay, 7=NonDisplay
    static constexpr uint8_t kColorMap[8] = {
        0xF4, // 0 → Green
        0xF4, // 1 → Green (reverse handled via highlight)
        0xF7, // 2 → White
        0xF7, // 3 → White (reverse)
        0xF4, // 4 → Green (underline)
        0xF4, // 5 → Green (reverse+underline)
        0x00, // 6 → NonDisplay
        0x00, // 7 → NonDisplay
    };
    return kColorMap[c3 & 0x07];
}

int DataStream5250Parser::rowColToOffset(uint8_t row, uint8_t col) const {
    // 5250 addresses are 1-indexed.  Row 0 or col 0 means "no change" (keep current).
    if (row == 0 || col == 0) return -1;
    int r = static_cast<int>(row) - 1;
    int c = static_cast<int>(col) - 1;
    if (r >= screen_.rows() || c >= screen_.cols()) return -1;
    return r * screen_.cols() + c;
}

} // namespace x3270
