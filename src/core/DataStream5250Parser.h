#pragma once
#include "ScreenBuffer.h"
#include "EbcdicCodec.h"
#include <cstdint>
#include <vector>
#include <functional>

namespace x3270 {

// ── 5250 Commands (first byte of GDS data after header) ───────────────────────
static constexpr uint8_t CMD5250_WTD              = 0x11; ///< Write To Display
static constexpr uint8_t CMD5250_CLEAR_UNIT        = 0x40; ///< Clear Unit
static constexpr uint8_t CMD5250_CLEAR_UNIT_ALT    = 0x20; ///< Clear Unit Alternate
static constexpr uint8_t CMD5250_WRITE_ERROR_CODE  = 0x21; ///< Write Error Code
static constexpr uint8_t CMD5250_SAVE_SCREEN       = 0x01; ///< Save Screen
static constexpr uint8_t CMD5250_RESTORE_SCREEN    = 0x02; ///< Restore Screen

// ── 5250 Orders (appear in WTD data stream) ───────────────────────────────────
static constexpr uint8_t ORD5250_SOH  = 0x01; ///< Start of Header
static constexpr uint8_t ORD5250_TD   = 0x02; ///< Transparent Data
static constexpr uint8_t ORD5250_SBA  = 0x11; ///< Set Buffer Address [row][col] 1-indexed
static constexpr uint8_t ORD5250_SF   = 0x1D; ///< Start of Field [FFW1][FFW2]
static constexpr uint8_t ORD5250_ATTR = 0x1C; ///< Set Attribute [attr]
static constexpr uint8_t ORD5250_IC   = 0x1F; ///< Insert Cursor
static constexpr uint8_t ORD5250_RA   = 0x3C; ///< Repeat to Address [row][col][char]
static constexpr uint8_t ORD5250_EA   = 0x3D; ///< Erase to Address [row][col]

// ── Field Format Word (FFW) — byte 0 (FFW1) bit masks ────────────────────────
static constexpr uint8_t FFW1_BYPASS      = 0x80; ///< Protected / bypass field
static constexpr uint8_t FFW1_DUP_ENABLE  = 0x40; ///< Allow dup key
static constexpr uint8_t FFW1_MDT         = 0x20; ///< Modified Data Tag
static constexpr uint8_t FFW1_SHIFT_MASK  = 0x18; ///< Shift type bits
static constexpr uint8_t FFW1_SHIFT_ALPHA = 0x00; ///< Alpha/shift
static constexpr uint8_t FFW1_SHIFT_NUM   = 0x10; ///< Numeric only
static constexpr uint8_t FFW1_DISP_MASK   = 0x06; ///< Display bits
static constexpr uint8_t FFW1_DISP_NORMAL = 0x00; ///< Normal
static constexpr uint8_t FFW1_DISP_INTEN  = 0x02; ///< High intensity
static constexpr uint8_t FFW1_DISP_NONE   = 0x04; ///< Non-display (hidden)
static constexpr uint8_t FFW1_DISP_SIGN   = 0x06; ///< Sign only

// ── DataStream5250Parser ──────────────────────────────────────────────────────
class DataStream5250Parser {
public:
    using AlarmCallback  = std::function<void()>;
    using UnlockCallback = std::function<void()>;
    using SendCallback   = std::function<void(const std::vector<uint8_t>&)>;

    DataStream5250Parser(ScreenBuffer& screen);

    void setAlarmCallback(AlarmCallback cb)  { alarmCb_  = std::move(cb); }
    void setUnlockCallback(UnlockCallback cb){ unlockCb_ = std::move(cb); }
    void setSendCallback(SendCallback cb)    { sendCb_   = std::move(cb); }

    /// Process a complete 5250 record from the host.
    /// The caller must strip the Telnet IAC EOR framing.
    /// This method handles GDS header stripping internally.
    void processRecord(const std::vector<uint8_t>& record);

private:
    enum class ParseState {
        Command,
        WCC1,
        WCC2,
        Data,
        SOH_Length,   SOH_Data,
        TD_LenHi,     TD_LenLo,   TD_Data,
        SBA_Row,
        SF_FFW1,      SF_FFW2,    SF_FCW_Hi, SF_FCW_Lo,
        ATTR_Byte,
        RA_Row,       RA_Col,     RA_Char,
        EA_Row,       EA_Col,
    };

    ScreenBuffer& screen_;

    AlarmCallback  alarmCb_;
    UnlockCallback unlockCb_;
    SendCallback   sendCb_;

    ParseState state_ { ParseState::Command };

    // Temporaries for multi-byte orders
    uint8_t  sohRemaining_  { 0 };
    uint16_t tdRemaining_   { 0 };
    uint8_t  raRow_         { 0 };
    uint8_t  raCol_         { 0 };

    // Active field attributes (accumulated from FFW)
    uint8_t  currentFFW1_   { 0 };
    uint8_t  currentFFW2_   { 0 };
    uint8_t  currentFgColor_{ 0 };

    void handleCommand(uint8_t cmd);
    void handleDataByte(uint8_t b);

    /// Map 5250 FFW bytes to a 3270-compatible ScreenBuffer attribute byte.
    uint8_t ffw1ToAttr(uint8_t ffw1) const;

    /// Map 5250 color attribute to IBM 3279 palette colour code.
    uint8_t mapColor(uint8_t colorAttr) const;

    /// Decode a 1-indexed 5250 [row][col] pair to a flat buffer offset.
    int rowColToOffset(uint8_t row, uint8_t col) const;
};

} // namespace x3270
