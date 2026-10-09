-- mouse_invert_int16.lua
-- Inverts both X and Y movement axes for HID mice that use int16 (little-endian)
-- relative movement values with a leading Report ID byte.
--
-- 8-byte report layout (Report ID 1):
--   byte 1: Report ID (0x01)
--   byte 2: button bitmask (bit0=left, bit1=right, bit2=middle)
--   byte 3: X low byte  } int16 LE relative X movement
--   byte 4: X high byte }
--   byte 5: Y low byte  } int16 LE relative Y movement
--   byte 6: Y high byte }
--   byte 7: scroll wheel (int8, relative)
--   byte 8: padding / reserved
--
-- Usage in injection.json:
--   { "ep_address": 81, "enable": true, "script_file": "scripts/mouse_invert_int16.lua" }

-- 把 int16 小端两字节取负（重建有符号值、饱和到 int16 范围、再编码回小端）。
local function negate_int16_le(lo, hi)
    -- Reconstruct signed int16 from little-endian bytes
    local v = lo | (hi << 8)
    if v >= 32768 then v = v - 65536 end
    -- Negate, clamped to int16 range
    v = -v
    if v < -32768 then v = -32768 end
    if v > 32767 then v = 32767 end
    -- Re-encode as little-endian unsigned
    if v < 0 then v = v + 65536 end
    return v & 0xFF, (v >> 8) & 0xFF
end

-- 反转 X/Y 两轴（各为 int16 小端）位移。
function transform(data, len)
    if len < 6 then return data, len end
    data[3], data[4] = negate_int16_le(data[3], data[4])  -- negate X
    data[5], data[6] = negate_int16_le(data[5], data[6])  -- negate Y
    return data, len
end
