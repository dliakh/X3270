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
    icOrderSeen_  = false;

    // Detect and strip GDS header.
    // Java-confirmed format (XI5250Emulator.receivedEOR):
    //   [0-1] = total len (big-endian)
    //   [2]   = 0x12
    //   [3]   = 0xA0
    //   [4-5] = 0x00 0x00 (reserved)
    //   [6]   = varHdrLen (typically 0x04)
    //   [7]   = flags
    //   [8]   = 0x00
    //   [9]   = opcode  (OUTPUT_ONLY=0x02, PUT_GET=0x03, etc.)
    //   [10+] = 5250 data payload (first byte = 5250 command, e.g. WTD=0x11)
    // Total header = 6 + varHdrLen bytes (= 10 when varHdrLen=4).
    if (len >= 10) {
        uint16_t encoded = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        if (data[2] == 0x12 && data[3] == 0xA0 &&
            encoded == static_cast<uint16_t>(len))
        {
            uint8_t varHdrLen = data[6];
            size_t  hdrLen    = 6 + static_cast<size_t>(varHdrLen);
            if (len <= hdrLen) return; // header only, no payload
            data += hdrLen;
            len  -= hdrLen;
            // Payload first byte is the 5250 command byte — leave state_=Command
            // so handleCommand() processes it normally.
        }
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        switch (state_) {
        case ParseState::Command:
            // The 5250 data stream uses ESC (0x04) as a command prefix.
            // Every top-level command is: ESC byte followed by the CMD byte.
            // (Within WTD data, ESC signals end of data and return to Command.)
            if (b == ESC5250) {
                state_ = ParseState::EscSeen;
            }
            // else: stray byte before ESC — skip it (mirrors reference "Ignoring record!")
            break;

        case ParseState::EscSeen:
            // b is the actual command byte following ESC
            handleCommand(b);
            break;

        case ParseState::WCC1:
            // CC1 (Control Code 1): handle MDT reset and field null operations.
            // CC1 & 0xE0 bits determine what to do with fields; CC1 & 0x80 = lock KB.
            // We fire the CC1 actions immediately, then save CC1 for reference.
            pendingCC1_ = b;
            handleCC1(b);
            state_ = ParseState::WCC2;
            break;

        case ParseState::WCC2:
            // CC2 bits — alarm and unlock are fired AFTER the WTD data loop ends.
            // Save CC2 and process it when we see ESC (end of WTD data) or EOR.
            pendingCC2_ = b;
            state_ = ParseState::Data;
            break;

        case ParseState::Data:
            handleDataByte(b);
            break;

        case ParseState::SOH_Length:
            sohRemaining_ = b;
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
            if (i + 1 < len) {
                uint8_t col = data[++i];
                int offset = rowColToOffset(row, col);
                if (offset >= 0) screen_.setBufferAddress(offset);
            }
            state_ = ParseState::Data;
            break;
        }

        case ParseState::SF_FirstByte:
            if ((b & 0xE0) != 0x20) {
                // FFW1 byte (input field)
                currentFFW1_ = b;
                state_ = ParseState::SF_FFW2;
            } else {
                // No FFW — b IS the attribute byte (output-only / protected field)
                pendingFieldAttr_       = b;  // has FA_PROTECTED(0x20) always set → isProtected()=true
                pending5250DisplayAttr_ = b;  // raw 5250 display attr for colour rendering
                state_ = ParseState::SF_LenHi;
            }
            break;
        case ParseState::SF_FFW2:
            currentFFW2_ = b;
            state_ = ParseState::SF_AfterFFW;
            break;
        case ParseState::SF_AfterFFW:
            if ((b & 0xE0) != 0x20) {
                // FCW1 — consume FCW2 and loop
                state_ = ParseState::SF_FCW2;
            } else {
                // b is the 5250 display attribute byte (0x20-0x3F).
                // Store FFW1-derived 3270-style protection bits in attr (for isProtected()),
                // and the actual 5250 display byte in fgColor (for colour rendering).
                pendingFieldAttr_       = ffw1ToAttr(currentFFW1_);
                pending5250DisplayAttr_ = b;
                state_ = ParseState::SF_LenHi;
            }
            break;
        case ParseState::SF_FCW2:
            state_ = ParseState::SF_AfterFFW;
            break;
        case ParseState::SF_LenHi:
            currentFieldLenHi_ = b;
            state_ = ParseState::SF_LenLo;
            break;
        case ParseState::SF_LenLo: {
            uint16_t flen = (static_cast<uint16_t>(currentFieldLenHi_) << 8) | b;
            // Pass pending5250DisplayAttr_ as fgColor so the renderer can use the raw
            // 5250 display attr for colour mapping without losing the protection bits in attr.
            screen_.startField(pendingFieldAttr_, pending5250DisplayAttr_, 0x00, 0x00, flen);
            state_ = ParseState::Data;
            break;
        }

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

        case ParseState::WEA_Skip:
            if (--sohRemaining_ == 0) state_ = ParseState::Data;
            break;

        case ParseState::WDSF_LenHi:
            tdRemaining_ = static_cast<uint16_t>(b) << 8;
            state_ = ParseState::WDSF_LenLo;
            break;
        case ParseState::WDSF_LenLo:
            tdRemaining_ |= b;
            tdRemaining_ = (tdRemaining_ >= 2) ? tdRemaining_ - 2 : 0;
            state_ = (tdRemaining_ > 0) ? ParseState::WDSF_Skip : ParseState::Data;
            break;
        case ParseState::WDSF_Skip:
            if (--tdRemaining_ == 0) state_ = ParseState::Data;
            break;

        case ParseState::MC_Row:
            raRow_ = b;
            state_ = ParseState::MC_Col;
            break;
        case ParseState::MC_Col: {
            int dest = rowColToOffset(raRow_, b);
            if (dest >= 0) screen_.setCursor(dest);
            state_ = ParseState::Data;
            break;
        }

        case ParseState::IC_Row:
            raRow_ = b;
            state_ = ParseState::IC_Col;
            break;
        case ParseState::IC_Col: {
            // IC sets the insert-cursor (pending cursor position after WTD completes).
            // The reference calls set_pending_insert which sets cursorPos.
            int dest = rowColToOffset(raRow_, b);
            if (dest >= 0) {
                screen_.setCursor(dest);
                icOrderSeen_ = true; // suppress cursor-home in deferred CC2 handling
            }
            state_ = ParseState::Data;
            break;
        }

        case ParseState::SkipOneThenCommand:
            state_ = ParseState::Command;
            break;

        case ParseState::WSF_LenHi:
            wsfBodyLen_ = static_cast<uint16_t>(b) << 8;
            state_ = ParseState::WSF_LenLo;
            break;
        case ParseState::WSF_LenLo:
            wsfBodyLen_ |= b;
            // Length field includes the 2 length bytes themselves; subtract to get body count.
            // Also subtract the 2 class+type bytes we already consumed.
            wsfBodyLen_ = (wsfBodyLen_ >= 4) ? wsfBodyLen_ - 4 : 0;
            state_ = (wsfBodyLen_ > 0) ? ParseState::WSF_Skip : ParseState::Command;
            if (wsfBodyLen_ == 0 && queryReplyCb_) queryReplyCb_(buildQueryReplyPayload());
            break;
        case ParseState::WSF_Skip:
            if (--wsfBodyLen_ == 0) {
                if (queryReplyCb_) queryReplyCb_(buildQueryReplyPayload());
                state_ = ParseState::Command;
            }
            break;
        }
    }

    // After processing all bytes in this record, fire any deferred CC2 actions.
    // (CC2 alarm/unlock apply after the WTD data stream ends.)
    if (state_ == ParseState::Data || state_ == ParseState::Command) {
        if (pendingCC2_ & 0x04) { if (alarmCb_)  alarmCb_(); }
        if (pendingCC2_ & 0x08) {
            if (unlockCb_) unlockCb_();
            // CC2 UNLOCK bit set: move cursor to first editable field when no explicit
            // IC order was seen in this WTD — matches tn5250_display_set_cursor_home()
            // behavior in the reference (session.c tn5250_session_write_to_display).
            if (!icOrderSeen_) screen_.setCursorToHome();
        }
        pendingCC2_ = 0;
    }
}

// ── CC1 handler ───────────────────────────────────────────────────────────────
// CC1 bits (reference tn5250_session_handle_cc1):
//   bits[7:5] == 0x00 → lock_kb=0 (unlock keyboard)
//   bits[7:5] == 0x40 → reset non-bypass MDT
//   bits[7:5] == 0x60 → reset all MDT
//   bits[7:5] == 0x80 → null non-bypass fields with MDT
//   bits[7:5] == 0xA0 → reset non-bypass MDT + null non-bypass
//   bits[7:5] == 0xC0 → reset non-bypass MDT + null MDT non-bypass
//   bits[7:5] == 0xE0 → reset all MDT + null non-bypass
void DataStream5250Parser::handleCC1(uint8_t cc1) {
    switch (cc1 & 0xE0) {
    case 0x00: break; // lock_kb = false — keyboard stays unlocked
    case 0x40: screen_.resetAllMDT(); break; // reset non-bypass MDT (simplified)
    case 0x60: screen_.resetAllMDT(); break; // reset all MDT
    case 0xA0: screen_.resetAllMDT(); break;
    case 0xC0: screen_.resetAllMDT(); break;
    case 0xE0: screen_.resetAllMDT(); break;
    default:   break;
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────────

void DataStream5250Parser::handleCommand(uint8_t cmd) {
    switch (cmd) {
    case CMD5250_WTD:
        state_ = ParseState::WCC1;
        break;

    case CMD5250_CLEAR_UNIT:
        screen_.eraseAll();
        state_ = ParseState::Command; // no further data; keyboard lock managed by CC2
        break;

    case CMD5250_CLEAR_UNIT_ALT:
        // Reference: reads one parameter byte (0x00 or 0x80) then clears screen.
        screen_.eraseAll();
        sohRemaining_ = 1; // reuse as skip counter
        state_ = ParseState::SkipOneThenCommand;
        break;

    case CMD5250_WRITE_ERROR_CODE:
        // Phase 1: ignore — host is sending an error indicator
        // (typically followed by a subsequent WTD to redraw the screen)
        state_ = ParseState::Command;
        break;

    case CMD5250_QUERY:  // 0xF3 = CMD_WRITE_STRUCTURED_FIELD
        // IBM i sends WRITE STRUCTURED FIELD with a 2-byte length, then class (0xD9),
        // then type (0x70 = QUERY). Read length, skip the body, fire query reply.
        // The WSF_LenHi/LenLo states parse the 2-byte length; WSF_Skip consumes body.
        wsfBodyLen_ = 0;
        state_ = ParseState::WSF_LenHi;
        break;

    case CMD5250_SAVE_SCREEN:    // 0x02
    case CMD5250_RESTORE_SCREEN: // 0x12
        // Not implemented — treat as no-op and return to Command state
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
    // Order bytes 0x01-0x1F per IBM 5250 Data Stream Programmer's Reference.
    // Bytes 0x20-0x3F are colour/attribute bytes (treated as data chars).
    // Bytes 0x40+ are displayable EBCDIC.
    // ESC (0x04) in the data stream signals the next command boundary.
    switch (b) {
    case ESC5250:  // 0x04: command delimiter — next byte is the actual command
        state_ = ParseState::EscSeen;
        break;
    case ORD5250_SOH:
        state_ = ParseState::SOH_Length;
        break;
    case ORD5250_RA:
        state_ = ParseState::RA_Row;
        break;
    case ORD5250_EA:
        state_ = ParseState::EA_Row;
        break;
    case ORD5250_TD:
        state_ = ParseState::TD_LenHi;
        break;
    case ORD5250_SBA:
        state_ = ParseState::SBA_Row;
        break;
    case ORD5250_WEA:
        // Write Extended Attribute: 2 bytes follow (type + value), skip them
        sohRemaining_ = 2;
        state_ = ParseState::WEA_Skip;
        break;
    case ORD5250_IC:
        // In 5250, IC takes two explicit bytes: [row][col] (1-indexed), unlike 3270 IC.
        // Reference: tn5250_session_insert_cursor reads row+col, then set_pending_insert.
        state_ = ParseState::IC_Row;
        break;
    case ORD5250_MC:
        state_ = ParseState::MC_Row;
        break;
    case ORD5250_WDSF:
        // Write Display Structured Field: 2-byte big-endian length then body
        state_ = ParseState::WDSF_LenHi;
        break;
    case ORD5250_SF:
        // Reset FFW accumulator before parsing the SF order
        currentFFW1_ = 0;
        currentFFW2_ = 0;
        currentFieldLenHi_ = 0;
        state_ = ParseState::SF_FirstByte;
        break;
    default:
        if (b >= 0x40) {
            // Normal EBCDIC display character
            screen_.writeChar(b);
        } else if (b >= 0x20) {
            // Colour/attribute byte in range 0x20-0x3F.
            // These are 5250 attribute characters that occupy a buffer position.
            // For now write them as the attribute byte (renderer will color-map).
            screen_.writeChar(b);
        }
        // else: unrecognised control byte < 0x20 — skip silently
        break;
    }
}

// ── Attribute mapping ─────────────────────────────────────────────────────────

uint8_t DataStream5250Parser::ffw1ToAttr(uint8_t ffw1) const {
    uint8_t attr = 0x00;

    // Protected / bypass field
    if (ffw1 & FFW1_BYPASS) {
        attr |= FA_PROTECTED | FA_NUMERIC; // bypass = protected skip field
    } else if (ffw1 & FFW1_SHIFT_NUM) {
        attr |= FA_NUMERIC;
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

// ── Query reply ───────────────────────────────────────────────────────────────
// Build the 61-byte query reply payload sent in response to CMD_WRITE_STRUCTURED_FIELD
// with SF type SF_5250_QUERY (0x70) or SF_5250_QUERY_STATION_STATE (0x72).
//
// Format from IBM 5250 Functions Reference §15.27.2 "5250 QUERY Command":
//   Byte  0-1 : Cursor row/col (0x00 0x00 — unused here)
//   Byte  2   : Inbound WSF AID (0x88)
//   Byte  3-4 : Query Reply length (0x00 0x3A = 58 bytes, NOT including bytes 0-2)
//   Byte  5   : Command class (0xD9)
//   Byte  6   : Command type (0x70 = Query Reply)
//   Byte  7   : Flag byte (0x80)
//   Byte  8-9 : Controller HW class (0x0600 = other WSF / 5250 emulator)
//   Byte 10-12: Controller code level (0x01 0x01 0x00 = version 1.1.0)
//   Byte 13-28: Reserved (0x00)
//   Byte 29   : Type = 0x01 (display emulation)
//   Byte 30-33: Device type in EBCDIC ("3477" or "3180")
//   Byte 34   : 0x00 (separator)
//   Byte 35-36: Device model in EBCDIC ("00" for FC, "02" for -2)
//   Byte 37   : Keyboard ID (0x02 = Standard keyboard)
//   Byte 38-39: 0x00 (extended KB ID + reserved)
//   Byte 40-43: Serial number (dummy)
//   Byte 44-45: Max input fields (0xFFFF = 65535)
//   Byte 46-48: Reserved (0x00)
//   Byte 49-50: Controller/Display capability (0x23 0x31)
//   Byte 51-60: Reserved (0x00)
//
// Reference: lib5250/session.c tn5250_session_query_reply()
std::vector<uint8_t> DataStream5250Parser::buildQueryReplyPayload() const {
    // EBCDIC digit encoding: '0'-'9' → 0xF0-0xF9
    const bool wide = (screen_.cols() >= 132);

    std::vector<uint8_t> p(61, 0x00);
    p[2]  = 0x88; // Inbound WSF AID
    p[4]  = 0x3A; // Query Reply length field = 58 (0x3A)
    p[5]  = 0xD9; // Command class
    p[6]  = 0x70; // Command type = Query Reply
    p[7]  = 0x80; // Flag byte
    p[8]  = 0x06; // Controller HW class hi  (0x0600 = other 5250 emulator)
    // p[9] = 0x00 already
    p[10] = 0x01; // Code level: version
    p[11] = 0x01; // release
    // p[12] = 0x00 modification; p[13..28] = reserved — already zero
    p[29] = 0x01; // Type = display emulation

    if (wide) {
        // Terminal type "IBM-3180-2": dev_type=3180, dev_model=2
        // EBCDIC: '3'=0xF3 '1'=0xF1 '8'=0xF8 '0'=0xF0  model '0'=0xF0 '2'=0xF2
        p[30]=0xF3; p[31]=0xF1; p[32]=0xF8; p[33]=0xF0;
        p[35]=0xF0; p[36]=0xF2;
    } else {
        // Terminal type "IBM-3477-FC": dev_type=3477, dev_model=0 (atoi("FC")=0)
        // EBCDIC: '3'=0xF3 '4'=0xF4 '7'=0xF7 '7'=0xF7  model '0'=0xF0 '0'=0xF0
        p[30]=0xF3; p[31]=0xF4; p[32]=0xF7; p[33]=0xF7;
        p[35]=0xF0; p[36]=0xF0;
    }
    // p[34] = 0x00 (separator between type and model)

    p[37] = 0x02; // Keyboard ID = Standard keyboard (0x82 = G keyboard)
    // p[38] = 0x00 extended KB ID; p[39] = 0x00 reserved
    p[40] = 0x00; // Serial number (4 bytes, dummy)
    p[41] = 0x61;
    p[42] = 0x50;
    p[43] = 0x00;
    p[44] = 0xFF; // Max input fields hi (65535)
    p[45] = 0xFF; // Max input fields lo
    // p[46] = 0x00 control unit customization; p[47..48] = reserved
    p[49] = 0x23; // Controller/Display capability byte 1
    p[50] = 0x31; // Controller/Display capability byte 2
    // p[51..60] = reserved, already zero

    return p;
}

} // namespace x3270
