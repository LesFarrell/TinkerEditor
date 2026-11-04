-- ziptool.lua - canonical library for the pure-Lua ZIP tool
-- Public API (exported at the bottom of the file):
--   M.ZipWriter      - streaming ZIP writer object (new, add_file, add_data, add_directory, add_tree, start_file_stream, write_chunk, finish_file_stream, close)
--   M.unzip_file     - extract a zip into a directory (handles ZIP64)
--   M.deflate_lz77   - small pure-Lua DEFLATE (LZ77) compressor (tests / portability)
--   M.inflate_deflate- matching inflater for deflate_lz77 output
--   M.crc32          - CRC32 checksum helper
-- Optional convenience helpers (if present):
--   M.list_zip       - list central-directory entries from a ZIP file
--   M.zip_dir        - create a zip from a directory
--   M.zip_paths      - create a zip from a list of paths
--
-- Notes:
-- - The implementation is intentionally small and dependency-free so it
--   can be used in environments without luarocks. Performance is not
--   the primary goal — correctness, portability and readability are.
-- - ZIP64 support is implemented where required (large sizes/offsets,
--   special extra fields). Many helpers include short docstrings and
--   argument/return descriptions.

local M = {}

-- (Begin library implementation)

local io_open = io.open
local os_time = os.time
local os_date = os.date
local tostring = tostring
local type = type

-- Little-endian pack helpers
-- Encode a 16-bit unsigned integer as little-endian bytes.
--
-- Args:
--   n (number): integer to encode (will be reduced modulo 2^16).
-- Returns:
--   (string) two-byte little-endian representation.
--
-- This small helper is used to build ZIP headers where fields are
-- stored in little-endian order.
local function u16(n)
    n = n % 65536
    return string.char(n % 256, math.floor(n / 256) % 256)
end

-- Encode a 32-bit unsigned integer as little-endian bytes.
--
-- Args:
--   n (number): integer to encode (will be reduced modulo 2^32).
-- Returns:
--   (string) four-byte little-endian representation.
--
-- Note: values greater than 2^32-1 will be reduced modulo 2^32; for
-- ZIP64 fields the `u64` helper should be used.
local function u32(n)
    n = n % 4294967296
    return string.char(n % 256, math.floor(n / 256) % 256, math.floor(n / 65536) % 256, math.floor(n / 16777216) % 256)
end

-- Encode a 64-bit unsigned integer as little-endian bytes.
--
-- Note: Lua numbers may lose precision for very large values, but this
-- function is sufficient for packing ZIP64 fields when used with integer
-- values produced by file offsets/sizes on typical platforms.
--
-- Args:
--   n (number): integer to encode (reduced modulo 2^64).
-- Returns:
--   (string) eight-byte little-endian representation.
--
-- The result is produced by concatenating two u32 values (low, high).
local function u64(n)
    n = n % 18446744073709551616
    local lo = n % 4294967296
    local hi = math.floor(n / 4294967296) % 4294967296
    return u32(lo) .. u32(hi)
end

-- Decode a little-endian 64-bit integer from a string.
--
-- Args:
--   s (string): byte string containing the value.
--   i (number): 1-based index in s where the 8-byte value starts.
-- Returns:
--   (number) decoded integer (may lose precision > 2^53).
--
local MAX32 = 0xFFFFFFFF
local MAX16 = 0xFFFF

-- Basic utility functions used by the writer/CLI
local ZipWriter = {}
ZipWriter.__index = ZipWriter

-- Minimal shell quoting helper for small platform probes and commands.
-- Escapes double quotes and wraps the string in quotes. Not a full
-- cross-shell quoting implementation but good enough for local use.
local function shell_quote(s)
    if not s then return '""' end
    return '"' .. tostring(s):gsub('"', '\\"') .. '"'
end

-- Normalize a file path to ZIP internal form (forward slashes, no drive
-- letter, no leading slashes).
--
-- Args:
--   name (string): original filesystem path.
-- Returns:
--   (string) normalized ZIP path.
local function zip_path(name)
    name = tostring(name or '')
    name = name:gsub('\\', '/')
    name = name:gsub('^%a:%/*', '')
    name = name:gsub('^/*', '')
    return name
end

-- Convert an os.date('*t') table into DOS date/time words used in ZIP
-- local file headers and central directory entries.
--
-- Args:
--   t (table): table returned by os.date('*t').
-- Returns:
--   (string, string) packed little-endian u16 words for time and date.
local function dos_datetime(t)
    local year = t.year or 1980
    if year < 1980 then year = 1980 end
    local month = t.month or 1
    local day = t.day or 1
    local hour = t.hour or 0
    local min = t.min or 0
    local sec = math.floor((t.sec or 0) / 2)
    local dos_time = hour * 2^11 + min * 2^5 + sec
    local dos_date = (year - 1980) * 2^9 + month * 2^5 + day
    return u16(dos_time), u16(dos_date)
end

-- CRC32 helpers
-- Build the CRC32 lookup table used by crc32/crc_update.
-- Returns a table of 256 entries for byte-wise CRC calculation.
--
-- This is executed once during module load; the resulting table is used
-- by the fast byte-wise crc32 implementation below.
local function build_crc_table()
    local poly = 0xEDB88320
    local tbl = {}
    for i = 0, 255 do
        local c = i
        for j = 1, 8 do
            if (c & 1) ~= 0 then
                c = poly ~ (c >> 1)
            else
                c = c >> 1
            end
        end
        tbl[i] = c
    end
    return tbl
end

local __crc_table = build_crc_table()
-- Compute CRC32 of a byte string.
--
-- Args:
--   data (string): bytes to compute CRC over.
-- Returns:
--   (number) CRC32 value as unsigned 32-bit number.
--
-- This returns the finalized CRC32 (XOR with 0xFFFFFFFF) suitable for
-- storage in ZIP metadata.
local function crc32(data)
    local crc = 0xFFFFFFFF
    for i = 1, #data do
        local b = data:byte(i)
        crc = (__crc_table[(crc ~ b) & 0xFF] ~ (crc >> 8)) & 0xFFFFFFFF
    end
    return (crc ~ 0xFFFFFFFF) & 0xFFFFFFFF
end

-- Incrementally update a CRC32 value with another chunk of data.
--
-- Args:
--   crc (number|nil): previous crc value, or nil to start a new CRC.
--   data (string): additional bytes to update the CRC with.
-- Returns:
--   (number) updated CRC32 accumulator (still in internal pre-finalized form).
--
-- The returned value is in the internal (pre-XOR) form so callers can
-- feed multiple chunks and finalize with `crc ~ 0xFFFFFFFF`.
local function crc_update(crc, data)
    crc = crc or 0xFFFFFFFF
    for i = 1, #data do
        local b = data:byte(i)
        crc = (__crc_table[(crc ~ b) & 0xFF] ~ (crc >> 8)) & 0xFFFFFFFF
    end
    return crc
end

-- Bit ops fallback (keeps use of native operators where available)
local has_bit = (bit32 ~= nil) or (jit and jit.bit) or false
local function bor(a,b) return (a|b) end
local function band(a,b) return (a&b) end
local function bxor(a,b) return (a~b) end
local function rshift(a,b) return (a>>b) end
local function lshift(a,b) return (a<<b) end
do
    local ok = pcall(function() local _ = 1 & 1 end)
    if not ok then
        if bit32 then
            bor = bit32.bor; band = bit32.band; bxor = bit32.bxor; rshift = bit32.rshift; lshift = bit32.lshift
        else
            bor = function(a,b) return a + b end
            band = function(a,b) return a % 256 end
            bxor = function(a,b) return a + b end
            rshift = function(a,b) return math.floor(a / 2^b) end
            lshift = function(a,b) return a * 2^b end
        end
    end
end

-- deflate fixed literals and deflate_lz77, inflate_deflate, and all other helpers
-- (For brevity here we re-use the same implementations as before.)

-- deflate_fixed_literals
-- Emit a DEFLATE block using fixed Huffman codes containing only literal
-- bytes (no LZ77 matches). This is used as a safe fallback to produce a
-- valid DEFLATE stream when forcing DEFLATE for data that doesn't compress
-- well with the LZ77 encoder.
--
-- Args:
--   data (string): input bytes to emit as literal symbols.
-- Returns:
--   (string) DEFLATE-compressed bytes using fixed Huffman codes.
--
-- Note: This function emits a single fixed-Huffman block and therefore
-- may not be size-optimal. It's chosen for simplicity and correctness
-- as a fallback when the compressor is forced to emit DEFLATE but the
-- LZ77 pass finds no beneficial matches.
local function deflate_fixed_literals(data)
    local out = {}
    local bitbuf = 0
    local bitlen = 0
    local function emit_bit(b)
        bitbuf = bitbuf + (b * (2 ^ bitlen))
        bitlen = bitlen + 1
        if bitlen == 8 then table.insert(out, string.char(bitbuf)); bitbuf = 0; bitlen = 0 end
    end
    local function emit_bits(val, n)
        for i = 0, n-1 do
            local bit = math.floor(val / (2 ^ i)) % 2
            emit_bit(bit)
        end
    end
    local function fixed_code(sym)
        if sym >= 0 and sym <= 143 then
            return sym + 48, 8
        elseif sym >= 144 and sym <= 255 then
            return sym + 256, 9
        elseif sym >= 256 and sym <= 279 then
            return sym - 256, 7
        elseif sym >= 280 and sym <= 287 then
            return sym - 88, 8
        end
        return nil
    end
    emit_bits(1, 1); emit_bits(1, 2)
    for i = 1, #data do
        local sym = data:byte(i)
        local code, l = fixed_code(sym)
        emit_bits(code, l)
    end
    local code, l = fixed_code(256)
    emit_bits(code, l)
    if bitlen > 0 then table.insert(out, string.char(bitbuf)) end
    return table.concat(out)
end

-- deflate_lz77
-- Compress `data` using a simple LZ77 matcher and emit a DEFLATE stream
-- using fixed Huffman codes. This is a compact pure-Lua compressor suitable
-- for tests and small archives. It supports `level` to influence window
-- and chain sizes (coarse-grained).
--
-- Args:
--   data (string): input bytes to compress.
--   level (number|nil): compression level 1-9 (defaults to 6).
-- Returns:
--   (string) DEFLATE-compressed bytes (fixed Huffman blocks).
--
-- Implementation note: This compressor uses a simple hash of two-byte
-- heads and a bounded backward search (chain) to find matches. It favors
-- clarity over speed and is intended for portability and tests.
local function deflate_lz77(data, level)
    level = tonumber(level) or 6
    local n = #data
    if n == 0 then return deflate_fixed_literals("") end
    local window_sizes = {256,512,1024,2048,4096,8192,16384,32768,32768}
    local chain_limits = {4,8,16,32,64,128,256,512,1024}
    local max_window = window_sizes[math.min(math.max(level,1),9)]
    local max_chain = chain_limits[math.min(math.max(level,1),9)]
    local out = {}
    local bitbuf = 0
    local bitlen = 0
    local function emit_bit(b)
        bitbuf = bitbuf + (b * (2 ^ bitlen))
        bitlen = bitlen + 1
        if bitlen == 8 then table.insert(out, string.char(bitbuf)); bitbuf = 0; bitlen = 0 end
    end
    local function emit_bits(val, n)
        for i = 0, n-1 do
            local bit = math.floor(val / (2 ^ i)) % 2
            emit_bit(bit)
        end
    end
    local length_base = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}
    local length_extra = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}
    local dist_base = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}
    local dist_extra = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}
    local function fixed_code(sym)
        if sym >= 0 and sym <= 143 then return sym + 48, 8
        elseif sym >= 144 and sym <= 255 then return sym + 256, 9
        elseif sym >= 256 and sym <= 279 then return sym - 256, 7
        elseif sym >= 280 and sym <= 287 then return sym - 88, 8 end
        return nil
    end
    local heads = {}
    local function add_head(pos)
        if pos >= n then return end
        local b1 = data:byte(pos)
        local b2 = data:byte(pos+1) or 0
        local key = b1 * 256 + b2
        local list = heads[key] or {}
        table.insert(list, pos)
        heads[key] = list
    end
    local i = 1
    emit_bits(1,1); emit_bits(1,2)
    while i <= n do
        local best_len = 0; local best_dist = 0
        if i < n then
            local b1 = data:byte(i)
            local b2 = data:byte(i+1) or 0
            local key = b1 * 256 + b2
            local list = heads[key]
            if list then
                local checked = 0
                for li = #list, 1, -1 do
                    local pos = list[li]
                    local dist = i - pos
                    if dist > max_window then break end
                    checked = checked + 1
                    if checked > max_chain then break end
                    local max_len = math.min(258, n - i + 1)
                    local len = 0
                    while len < max_len and data:byte(pos + len) == data:byte(i + len) do len = len + 1 end
                    if len > best_len and len >= 3 then best_len = len; best_dist = dist end
                    if best_len >= 258 then break end
                end
            end
        end
        if best_len >= 3 then
            local len = best_len
            local li = 1
            while li <= #length_base and not (len >= length_base[li] and (li == #length_base or len < length_base[li+1])) do li = li + 1 end
            local code = 256 + li
            local codeval, codelen = fixed_code(code)
            emit_bits(codeval, codelen)
            local extra = len - length_base[li]
            if length_extra[li] and length_extra[li] > 0 then emit_bits(extra, length_extra[li]) end
            local dist = best_dist
            local di = 1
            while di <= #dist_base and not (dist >= dist_base[di] and (di == #dist_base or dist < dist_base[di+1])) do di = di + 1 end
            emit_bits(di-1, 5)
            local dext = dist - dist_base[di]
            if dist_extra[di] and dist_extra[di] > 0 then emit_bits(dext, dist_extra[di]) end
            for k = 0, best_len-1 do add_head(i + k) end
            i = i + best_len
        else
            local sym = data:byte(i)
            local codeval, codelen = fixed_code(sym)
            emit_bits(codeval, codelen)
            add_head(i)
            i = i + 1
        end
    end
    local codeval, codelen = fixed_code(256)
    emit_bits(codeval, codelen)
    if bitlen > 0 then table.insert(out, string.char(bitbuf)) end
    return table.concat(out)
end

-- Inflate (simple support for stored and fixed)
-- Build a decode table for fixed Huffman codes used by our inflater.
-- Returns a table keyed by code length mapping code->symbol.
--
-- The returned structure maps bit-length -> {code->symbol} so the
-- inflater can accumulate bits LSB-first and check for a matching
-- symbol quickly.
local function build_fixed_decode()
    local table_by_len = {}
    for sym = 0, 287 do
        local code, len
        if sym >= 0 and sym <= 143 then code = sym + 48; len = 8
        elseif sym >= 144 and sym <= 255 then code = sym + 256; len = 9
        elseif sym >= 256 and sym <= 279 then code = sym - 256; len = 7
        elseif sym >= 280 and sym <= 287 then code = sym - 88; len = 8
        end
        if code and len then table_by_len[len] = table_by_len[len] or {}; table_by_len[len][code] = sym end
    end
    return table_by_len
end
local fixed_table = build_fixed_decode()

-- BitReader: small utility to read LSB-first bit sequences from a string.
--
-- Returns an object with methods `read_bits(n)` and `read_bit()`.
--
-- The BitReader reads bytes and exposes a little-endian bit buffer
-- consistent with DEFLATE's LSB-first bit ordering.
local function BitReader(data)
    local o = {data = data, pos = 1, bitbuf = 0, bitlen = 0}
    local function fill(self)
        if self.pos <= #self.data then
            self.bitbuf = self.bitbuf + ((self.data:byte(self.pos) or 0) * (2 ^ self.bitlen))
            self.bitlen = self.bitlen + 8
            self.pos = self.pos + 1
        end
    end
    function o:read_bits(n)
        while self.bitlen < n do fill(self) end
        local val = 0
        for i = 0, n-1 do val = val + (((self.bitbuf // (2 ^ i)) % 2) * (2 ^ i)) end
        self.bitbuf = math.floor(self.bitbuf / (2 ^ n))
        self.bitlen = self.bitlen - n
        return val
    end
    function o:read_bit() return self:read_bits(1) end
    return o
end

-- Decode a single symbol from a BitReader using the precomputed fixed
-- Huffman decode table. Returns the symbol or (nil, error).
--
-- The decoder accumulates bits up to 9 bits (max fixed-code length) and
-- consults the precomputed table by length to resolve a symbol. If no
-- symbol matches within 9 bits an error is returned.
local function decode_fixed_symbol(br)
    local acc = 0
    for l = 1, 9 do
        local b = br:read_bit()
        acc = acc + (b * (2 ^ (l-1)))
        local t = fixed_table[l]
        if t and t[acc] then return t[acc] end
    end
    return nil, 'invalid symbol'
end

-- Inflate a DEFLATE compressed byte string. Supports stored blocks and
-- fixed-Huffman blocks (the types produced by our deflater).
--
-- Args:
--   data (string): compressed DEFLATE data (raw deflate stream, no zlib)
-- Returns:
--   (string) decompressed bytes, or (nil, err) on failure.
--
-- This inflater intentionally supports only the subset emitted by this
-- module's compressor: stored blocks and fixed-huffman compressed blocks
-- with literal/length & distance pairs. It is not a full-featured DEFLATE
-- implementation but is sufficient for library tests and round-trips.
local function inflate_deflate(data)
    local br = BitReader(data)
    local out = {}
    local length_base = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}
    local length_extra = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}
    local dist_base = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}
    local dist_extra = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}
    while true do
        local bfinal = br:read_bit()
        local btype = br:read_bits(2)
        if btype == 0 then
            while br.bitlen % 8 ~= 0 do br:read_bit() end
            local lo = br:read_bits(8); local hi = br:read_bits(8)
            local len = lo + hi * 256
            local nlo = br:read_bits(8); local nhi = br:read_bits(8)
            local nlen = nlo + nhi * 256
            if (len ~ nlen) ~= 0xFFFF then return nil, 'stored len mismatch' end
            for i = 1, len do table.insert(out, string.char(br:read_bits(8))) end
        elseif btype == 1 then
            while true do
                local sym = decode_fixed_symbol(br)
                if not sym then return nil, 'decode error' end
                if sym < 256 then table.insert(out, string.char(sym))
                elseif sym == 256 then break
                else
                    local lenidx = sym - 256
                    local lb = length_base[lenidx]
                    local le = length_extra[lenidx]
                    local extra = 0
                    if le and le > 0 then extra = br:read_bits(le) end
                    local length = lb + extra
                    local distbits = br:read_bits(5)
                    local dist = dist_base[distbits+1]
                    local de = dist_extra[distbits+1]
                    local dext = 0
                    if de and de > 0 then dext = br:read_bits(de) end
                    dist = dist + dext
                    local outlen = #out
                    for k = 1, length do
                        local pos = outlen - dist + k
                        if pos < 1 then return nil, 'dist out of range' end
                        table.insert(out, out[pos])
                    end
                end
            end
        else
            return nil, 'unsupported BTYPE'
        end
        if bfinal == 1 then break end
    end
    return table.concat(out)
end

-- helper read le16/le32
-- Read a little-endian 16-bit unsigned integer from string `s` at index `i`.
--
-- Args:
--   s (string): source bytes.
--   i (number): 1-based index into s.
-- Returns:
--   (number) decoded 16-bit integer.
local function le16(s, i)
    i = i or 1
    local a = s:byte(i) or 0
    local b = s:byte(i+1) or 0
    return a + b * 256
end
-- Read a little-endian 32-bit unsigned integer from string `s` at index `i`.
--
-- Args:
--   s (string): source bytes.
--   i (number): 1-based index into s.
-- Returns:
--   (number) decoded 32-bit integer.
local function le32(s, i)
    i = i or 1
    local a = s:byte(i) or 0
    local b = s:byte(i+1) or 0
    local c = s:byte(i+2) or 0
    local d = s:byte(i+3) or 0
    return a + b * 256 + c * 65536 + d * 16777216
end

-- Read a little-endian 64-bit unsigned integer from string `s` at index `i`.
--
-- Args:
--   s (string): byte string containing the value.
--   i (number): 1-based index in s where the 8-byte value starts.
-- Returns:
--   (number) decoded integer (may lose precision > 2^53).
--
-- Uses le32 to reconstruct the low/high 32-bit halves. Must come after le32
-- is declared above it (as a local), not before: a `local function` earlier
-- in the file that calls `le32` would resolve to the global le32, which is
-- never assigned.
local function le64(s, i)
    i = i or 1
    local lo = le32(s, i)
    local hi = le32(s, i+4)
    return lo + hi * 4294967296
end

-- find_eocd_and_zip64
-- Locate the End Of Central Directory (EOCD) for a ZIP file and, if
-- present, read ZIP64 EOCD fields. Returns a table with entries, cd_size,
-- and cd_offset (numeric), or (nil, err).
--
-- Args:
--   f (file): a seekable file handle opened in binary mode.
-- Returns:
--   (table|nil, string|nil)
local function find_eocd_and_zip64(f)
    local size = f:seek('end')
    local scan = math.min(65536 + 22, size)
    f:seek('end', -scan)
    local tail = f:read(scan)
    local sig = string.char(0x50,0x4b,0x05,0x06)
    local pos = tail:find(sig, 1, true)
    if not pos then return nil, 'EOCD not found' end
    local e = tail:sub(pos, pos + 22 - 1)
    local total_entries = le16(e, 11)
    local cd_size = le32(e, 13)
    local cd_offset = le32(e, 17)
    if total_entries == 0xFFFF or cd_size == 0xFFFFFFFF or cd_offset == 0xFFFFFFFF then
        local loc_sig = string.char(0x50,0x4b,0x06,0x07)
        local loc_pos = tail:find(loc_sig, 1, true)
        if not loc_pos then return nil, 'ZIP64 locator not found' end
        local locator = tail:sub(loc_pos, loc_pos + 20 - 1)
        if not locator or #locator < 20 then return nil, 'ZIP64 locator truncated' end
        local zip64_eocd_off = le64(locator, 9)
        if zip64_eocd_off < 0 or zip64_eocd_off > size then return nil, 'ZIP64 EOCD offset invalid' end
        f:seek('set', zip64_eocd_off)
        local sig4 = f:read(4)
        if sig4 ~= string.char(0x50,0x4b,0x06,0x06) then return nil, 'ZIP64 EOCD signature not found' end
        local size_bytes = f:read(8)
        local eocd_size = le64(size_bytes,1)
        local eocd_rest = f:read(eocd_size)
        if not eocd_rest or #eocd_rest < 40 then return nil, 'ZIP64 EOCD truncated' end
        local total_entries64 = le64(eocd_rest, 13)
        local cd_size64 = le64(eocd_rest, 29)
        local cd_offset64 = le64(eocd_rest, 37)
        return {entries = total_entries64, cd_size = cd_size64, cd_offset = cd_offset64}
    end
    return {entries = total_entries, cd_size = cd_size, cd_offset = cd_offset}
end

-- unzip_file implementation
-- helper: platform and filesystem helpers used by unzip/create routines
-- Return true when running on Windows (path separator is '\').
local function is_windows()
    return package.config:sub(1,1) == '\\'
end

-- Return true if a given filesystem path exists and is readable as a file.
local function file_exists(path)
    local f = io_open(path, 'rb')
    if f then f:close(); return true end
    return false
end

-- Create directory path (including parents). Uses platform-appropriate
-- shell command to avoid adding dependencies on luarocks/fs libs.
local function make_dir(path)
    if path == '' then return true end
    if is_windows() then
        return os.execute('mkdir ' .. shell_quote(path) .. ' > nul 2>&1')
    else
        return os.execute('mkdir -p ' .. shell_quote(path))
    end
end

local function unzip_file(zip_path, out_dir)
    out_dir = out_dir or '.'
    local f, err = io_open(zip_path, 'rb')
    if not f then return nil, err end
    local eocd, eerr = find_eocd_and_zip64(f)
    if not eocd then f:close(); return nil, eerr end
    f:seek('set', eocd.cd_offset)
    local cd = f:read(eocd.cd_size)
    if not cd or #cd < 46 then f:close(); return nil, 'central directory truncated' end
    local i = 1
    local entries = {}
    while i <= #cd do
        local sig = cd:sub(i, i+3)
        if sig ~= string.char(0x50,0x4b,0x01,0x02) then break end
        local flags = le16(cd, i+8)
        local method = le16(cd, i+10)
        local crc = le32(cd, i+16)
        local comp_size = le32(cd, i+20)
        local size = le32(cd, i+24)
        local name_len = le16(cd, i+28)
        local extra_len = le16(cd, i+30)
        local comment_len = le16(cd, i+32)
        local header_offset = le32(cd, i+42)
        local name = cd:sub(i+46, i+45+name_len)
        local extra = cd:sub(i+46+name_len, i+45+name_len+extra_len)
        if (comp_size == 0xFFFFFFFF or size == 0xFFFFFFFF or header_offset == 0xFFFFFFFF) and #extra > 0 then
            local j = 1
            while j <= #extra do
                local eid = le16(extra, j)
                local elen = le16(extra, j+2)
                if eid == 0x0001 then
                    local payload = extra:sub(j+4, j+3+elen)
                    local pidx = 1
                    if size == 0xFFFFFFFF then size = le64(payload, pidx); pidx = pidx + 8 end
                    if comp_size == 0xFFFFFFFF then comp_size = le64(payload, pidx); pidx = pidx + 8 end
                    if header_offset == 0xFFFFFFFF then header_offset = le64(payload, pidx); pidx = pidx + 8 end
                    break
                end
                j = j + 4 + elen
            end
        end
        table.insert(entries, {name = name, method = method, crc = crc, comp = comp_size, size = size, header_offset = header_offset, flags = flags})
        i = i + 46 + name_len + extra_len + comment_len
    end
    local function sanitize_name(n)
        n = n or ''
        n = n:gsub('^/*', '')
        n = n:gsub('^%a:[\\/]+', '')
        local parts = {}
        for part in n:gmatch('[^/]+') do
            if part == '..' then if #parts > 0 then table.remove(parts) end
            elseif part ~= '.' and part ~= '' then table.insert(parts, part) end
        end
        return table.concat(parts, '/')
    end
    for _, e in ipairs(entries) do
        local sname = sanitize_name(e.name)
        if sname == '' then goto continue end
        local sep = package.config:sub(1,1)
        local outpath = sname:gsub('/', sep)
        local full = out_dir .. (out_dir:sub(-1) == sep and '' or sep) .. outpath
        local dir = full:match('^(.+)[\\/]') or out_dir
        make_dir(dir)
        f:seek('set', e.header_offset)
        local lh = f:read(30)
        if not lh or #lh < 30 then f:close(); return nil, 'local header truncated' end
        local lnam = le16(lh, 27)
        local lextra = le16(lh, 29)
        local data_offset = e.header_offset + 30 + lnam + lextra
        f:seek('set', data_offset)
        local data = f:read(e.comp)
        if not data and e.comp > 0 then f:close(); return nil, 'compressed data truncated' end
        local outdata, oerr
        if e.method == 0 then outdata = data or ''
        elseif e.method == 8 then outdata, oerr = inflate_deflate(data); if not outdata then f:close(); return nil, oerr end
        else f:close(); return nil, 'unsupported method '..tostring(e.method) end
        if e.name:sub(-1) ~= '/' then
            local wf, werr = io_open(full, 'wb')
            if not wf then f:close(); return nil, werr end
            wf:write(outdata)
            wf:close()
            local got = crc32(outdata)
            if got ~= e.crc then f:close(); return nil, string.format('CRC mismatch for %s: expected %08X got %08X', e.name, e.crc, got) end
        else
            make_dir(full)
        end
        ::continue::
    end
    f:close()
    return true
end

-- The rest of the library (ZipWriter implementation, streaming, add_tree, etc.)
-- For brevity, re-use the previous ZipWriter methods by defining them below.

-- Create a new ZipWriter instance for writing an archive.
--
-- Args:
--   outpath (string): output zip file path to write to.
--   opts (table|nil): options table where:
--     - compress (bool): enable compression (default false).
--     - level (number): compression level 1-9 (default 6).
--     - force_deflate (bool): force method=8 DEFLATE even if no savings.
-- Returns:
--   (ZipWriter, nil) on success, or (nil, err) on failure.
function ZipWriter.new(outpath, opts)
    local f, err = io_open(outpath, 'wb')
    if not f then return nil, err end
    opts = opts or {}
    local self = setmetatable({f = f, entries = {}, offset = 0, compress = not not opts.compress, level = tonumber(opts.level) or 6, force_deflate = not not opts.force_deflate}, ZipWriter)
    return self
end

function ZipWriter:_write(s)
    self.f:write(s)
    self.offset = self.offset + #s
end

-- Add a file from the filesystem into the archive under `arcname`.
-- The method chooses streaming vs in-memory path depending on file size.
--
-- Args:
--   path (string): path to the source file on disk.
--   arcname (string|nil): name to store inside the archive (defaults to basename).
-- Returns:
--   true on success or (nil, err) on failure.
function ZipWriter:add_file(path, arcname)
    arcname = arcname or path
    arcname = zip_path(arcname)
    local f, err = io_open(path, 'rb')
    if not f then return nil, err end
    local sz = f:seek('end')
    f:seek('set', 0)
    if sz > 1024*1024 then
        -- Large file: stream through the writer to avoid buffering whole file
        self:start_file_stream(arcname)
        while true do
            local chunk = f:read(65536)
            if not chunk then break end
            self:write_chunk(chunk)
        end
        f:close()
        return self:finish_file_stream()
    else
        -- Small file: read into memory and compress at once
        local data = f:read('*a')
        f:close()
        return self:add_data(arcname, data)
    end
end

-- Add raw data as a named entry in the archive. This is the common
-- non-streaming path used for small files and programmatically generated
-- byte strings.
--
-- Args:
--   name (string): name to store inside the archive.
--   data (string): raw uncompressed bytes.
--   modtime (optional): ignored currently but reserved for future use.
-- Returns:
--   true on success or (nil, err) on failure.
function ZipWriter:add_data(name, data, modtime)
    if type(name) ~= 'string' then return nil, 'name must be string' end
    name = zip_path(name)
    local crc = crc32(data)
    local size = #data
    local method = 0
    local outdata = data
    local comp_size = size
    if self.compress or self.force_deflate then
        local comp = deflate_lz77(data, self.level)
    -- debug (controlled by module flag)
    if M and M.debug and io and io.stderr and io.stderr.write then io.stderr:write(string.format('DEBUG add_data: name=%s compress=%s force_deflate=%s complen=%d\n', tostring(name), tostring(self.compress), tostring(self.force_deflate), comp and #comp or 0)) end
        if comp and #comp > 0 then
            outdata = comp; comp_size = #outdata; method = 8
        elseif self.force_deflate then
            -- Force DEFLATE even if compression yields no savings: use fixed-literal block as fallback
            if not comp or #comp == 0 then
                comp = deflate_fixed_literals(data)
            end
            outdata = comp; comp_size = #outdata; method = 8
        end
    end
    local dos_time, dos_date = dos_datetime(os_date('*t'))
    local local_header = table.concat{
        string.char(0x50,0x4b,0x03,0x04), u16(20), u16(0), u16(method), dos_time, dos_date, u32(crc), u32(comp_size), u32(size), u16(#name), u16(0), name
    }
    local header_offset = self.offset
    self:_write(local_header)
    if comp_size > 0 then self:_write(outdata) end
    local extra = ''
    local cd_comp_size = comp_size
    local cd_size = size
    local cd_offset = header_offset
    if comp_size > MAX32 or size > MAX32 or header_offset > MAX32 then
        local zbuf = {}
        if size > MAX32 then table.insert(zbuf, u64(size)) else table.insert(zbuf, u64(0)) end
        if comp_size > MAX32 then table.insert(zbuf, u64(comp_size)) else table.insert(zbuf, u64(0)) end
        if header_offset > MAX32 then table.insert(zbuf, u64(header_offset)) else table.insert(zbuf, u64(0)) end
        extra = string.char(0x01,0x00) .. u16(#table.concat(zbuf)) .. table.concat(zbuf)
        cd_comp_size = math.min(comp_size, MAX32)
        cd_size = math.min(size, MAX32)
        cd_offset = math.min(header_offset, MAX32)
    end
    local central = table.concat{
        string.char(0x50,0x4b,0x01,0x02), u16(45), u16(45), u16(0), u16(method), dos_time, dos_date, u32(crc), u32(cd_comp_size), u32(cd_size), u16(#name), u16(#extra), u16(0), u16(0), u16(0), u32(0), u32(cd_offset), name, extra
    }
    table.insert(self.entries, central)
    return true
end

-- Add a directory entry to the archive (name ends with '/'). Directory
-- entries have zero sizes and are represented specially in ZIP central
-- directory.
function ZipWriter:add_directory(name)
    name = zip_path(name)
    if not name:match('/$') then name = name .. '/' end
    local dos_time, dos_date = dos_datetime(os_date('*t'))
    local header_offset = self.offset
    local local_header = table.concat{string.char(0x50,0x4b,0x03,0x04), u16(20), u16(0), u16(0), dos_time, dos_date, u32(0), u32(0), u32(0), u16(#name), u16(0), name}
    self:_write(local_header)
    local central = table.concat{string.char(0x50,0x4b,0x01,0x02), u16(20), u16(20), u16(0), u16(0), dos_time, dos_date, u32(0), u32(0), u32(0), u16(#name), u16(0), u16(0), u16(0), u16(0), u32(16), u32(header_offset), name}
    table.insert(self.entries, central)
    return true
end

-- Finalize the archive by writing the central directory and EOCD records.
-- Handles ZIP64 extensions when needed (many entries or large offsets).
function ZipWriter:close()
    local central_offset = self.offset
    local cd = table.concat(self.entries)
    self:_write(cd)
    local central_size = self.offset - central_offset
    local need_zip64 = false
    if #self.entries > MAX16 or central_size > MAX32 or central_offset > MAX32 then need_zip64 = true end
    if need_zip64 then
        local zip64_eocd = table.concat{string.char(0x50,0x4b,0x06,0x06), u64(44), u16(45), u16(45), u32(0), u32(0), u64(#self.entries), u64(#self.entries), u64(central_size), u64(central_offset)}
        self:_write(zip64_eocd)
        local zip64_locator = table.concat{string.char(0x50,0x4b,0x06,0x07), u32(0), u64(self.offset - #zip64_eocd), u32(1)}
        self:_write(zip64_locator)
    end
    local eocd = table.concat{string.char(0x50,0x4b,0x05,0x06), u16(0), u16(0), u16(need_zip64 and 0xFFFF or #self.entries), u16(need_zip64 and 0xFFFF or #self.entries), u32(need_zip64 and 0xFFFFFFFF or central_size), u32(need_zip64 and 0xFFFFFFFF or central_offset), u16(0)}
    self:_write(eocd)
    self.f:close()
    self.f = nil
    return true
end

-- enumerate helpers
local function is_windows()
    return package.config:sub(1,1) == '\\'
end
local function file_exists(path)
    local f = io_open(path, 'rb')
    if f then f:close(); return true end
    return false
end
local function make_dir(path)
    if path == '' then return true end
    if is_windows() then return os.execute('mkdir ' .. shell_quote(path) .. ' > nul 2>&1') else return os.execute('mkdir -p ' .. shell_quote(path)) end
end
local function le16_local(s, i)
    i = i or 1
    local a = s:byte(i) or 0
    local b = s:byte(i+1) or 0
    return a + b * 256
end
local function le32_local(s, i)
    i = i or 1
    local a = s:byte(i) or 0
    local b = s:byte(i+1) or 0
    local c = s:byte(i+2) or 0
    local d = s:byte(i+3) or 0
    return a + b * 256 + c * 65536 + d * 16777216
end

-- re-use earlier unzip implementation's dependency on le16/le32 names
-- simple re-assign
le16 = le16_local; le32 = le32_local; make_dir = make_dir; file_exists = file_exists

-- Streaming and rest functions (start_file_stream, write_chunk, finish_file_stream, add_tree)
-- Start streaming a file into the archive. Call `write_chunk` repeatedly
-- then `finish_file_stream` to finalize the entry. Streaming allows large
-- files to be added without buffering the entire file in memory.
--
-- Args:
--   name (string): archive entry name.
-- Returns:
--   true on success.
function ZipWriter:start_file_stream(name)
    name = zip_path(name)
    local dos_time, dos_date = dos_datetime(os_date('*t'))
    local local_header = table.concat{string.char(0x50,0x4b,0x03,0x04), u16(20), u16(0x08), u16(0), dos_time, dos_date, u32(0), u32(0), u32(0), u16(#name), u16(0), name}
    local header_offset = self.offset
    self:_write(local_header)
    self._stream = {name = name, offset = header_offset, crc = 0xFFFFFFFF, size = 0, method = 0}
    if self.compress or self.force_deflate then
        self._stream.method = 8
        local buffer = {}
        local function feed(chunk) table.insert(buffer, chunk); return '' end
        local function finish() return deflate_lz77(table.concat(buffer), self.level) end
        self._stream._feed = feed; self._stream._finish = finish
    end
    return true
end

-- Write a chunk into the currently active file stream.
--
-- Args:
--   chunk (string): data bytes to append to the streamed entry.
-- Returns:
--   true on success or (nil, err).
function ZipWriter:write_chunk(chunk)
    if not self._stream then return nil, 'no active stream' end
    local crc = self._stream.crc
    for i = 1, #chunk do
        local b = chunk:byte(i)
        crc = (__crc_table[(crc ~ b) & 0xFF] ~ (crc >> 8)) & 0xFFFFFFFF
    end
    self._stream.crc = crc
    self._stream.size = self._stream.size + #chunk
    if self._stream.method == 8 then self._stream._feed(chunk) else self:_write(chunk) end
    return true
end

-- Finish the active file stream: write any remaining compressed bytes and
-- append the data descriptor and central-directory record for the entry.
--
-- Returns:
--   true on success or (nil, err).
function ZipWriter:finish_file_stream()
    if not self._stream then return nil, 'no active stream' end
    local crc = (self._stream.crc ~ 0xFFFFFFFF) & 0xFFFFFFFF
    local size = self._stream.size
    local comp_size = size
    local header_offset = self._stream.offset
    if self._stream.method == 8 then local comp = self._stream._finish(); comp_size = #comp; self:_write(comp) end
    local desc = table.concat{string.char(0x50,0x4b,0x07,0x08), u32(crc), u32(comp_size), u32(size)}
    self:_write(desc)
    local dos_time, dos_date = dos_datetime(os_date('*t'))
    local extra = ''
    local cd_comp_size = comp_size
    local cd_size = size
    local cd_offset = header_offset
    if comp_size > MAX32 or size > MAX32 or header_offset > MAX32 then
        local zbuf = {}
        if size > MAX32 then table.insert(zbuf, u64(size)) else table.insert(zbuf, u64(0)) end
        if comp_size > MAX32 then table.insert(zbuf, u64(comp_size)) else table.insert(zbuf, u64(0)) end
        if header_offset > MAX32 then table.insert(zbuf, u64(header_offset)) else table.insert(zbuf, u64(0)) end
        extra = string.char(0x01,0x00) .. u16(#table.concat(zbuf)) .. table.concat(zbuf)
        cd_comp_size = math.min(comp_size, MAX32)
        cd_size = math.min(size, MAX32)
        cd_offset = math.min(header_offset, MAX32)
    end
    local method = self._stream.method or 0
    local ver_made = 20; local ver_needed = 20
    if #extra > 0 then ver_made = 45; ver_needed = 45 end
    local central = table.concat{string.char(0x50,0x4b,0x01,0x02), u16(ver_made), u16(ver_needed), u16(0x08), u16(method), dos_time, dos_date, u32(crc), u32(cd_comp_size), u32(cd_size), u16(#self._stream.name), u16(#extra), u16(0), u16(0), u16(0), u32(0), u32(cd_offset), self._stream.name, extra}
    table.insert(self.entries, central)
    self._stream = nil
    return true
end

-- Recursively add files and directories from `path` into the archive under
-- the `base` archive name. Uses platform-appropriate `dir`/`find` probes to
-- enumerate files. Returns true on success.
function ZipWriter:add_tree(path, base)
    base = base or ''
    path = path:gsub('[\\/]+$', '')
    local files = {}
    local dirs = {}
    -- use simple os commands for enumeration as before
    if is_windows() then
        local p = io.popen('dir /b /s /a:-D ' .. shell_quote(path))
        if p then for line in p:lines() do table.insert(files, line) end p:close() end
        local pd = io.popen('dir /b /s /ad ' .. shell_quote(path))
        if pd then for line in pd:lines() do table.insert(dirs, line) end pd:close() end
    else
        local p = io.popen('find ' .. shell_quote(path) .. ' -type f -print')
        if p then for line in p:lines() do table.insert(files, line) end p:close() end
        local pd = io.popen('find ' .. shell_quote(path) .. ' -type d -print')
        if pd then for line in pd:lines() do table.insert(dirs, line) end pd:close() end
    end
    table.sort(dirs, function(a,b) return #a < #b end)
    for _, d in ipairs(dirs) do
        local rel = d:sub(#path+2)
        if rel and #rel > 0 then local arc = (base == '' and rel) or (base .. '/' .. rel); arc = zip_path(arc); self:add_directory(arc) end
    end
    for _, f in ipairs(files) do
        local rel = f:sub(#path+2)
        local arc = (base == '' and rel) or (base .. '/' .. rel)
        arc = zip_path(arc)
        self:add_file(f, arc)
    end
    return true
end

-- list_zip: returns table of entries {name, size, comp, method, crc, flags, header_offset, mtime}
-- list_zip: return central-directory entries for `zip_path`.
--
-- Returns a table of entry tables with fields: name, size, comp, method,
-- crc, flags, header_offset, mtime.
local function list_zip(zip_path)
    local f, err = io_open(zip_path, 'rb')
    if not f then return nil, err end
    local eocd, eerr = find_eocd_and_zip64(f)
    if not eocd then f:close(); return nil, eerr end
    f:seek('set', eocd.cd_offset)
    local cd = f:read(eocd.cd_size)
    if not cd or #cd < 46 then f:close(); return nil, 'central directory truncated' end
    local i = 1
    local entries = {}
    while i <= #cd do
        local sig = cd:sub(i, i+3)
        if sig ~= string.char(0x50,0x4b,0x01,0x02) then break end
        local flags = le16(cd, i+8)
        local method = le16(cd, i+10)
        local crc = le32(cd, i+16)
        local comp_size = le32(cd, i+20)
        local size = le32(cd, i+24)
        local name_len = le16(cd, i+28)
        local extra_len = le16(cd, i+30)
        local comment_len = le16(cd, i+32)
        local header_offset = le32(cd, i+42)
        local name = cd:sub(i+46, i+45+name_len)
        local extra = cd:sub(i+46+name_len, i+45+name_len+extra_len)
        if (comp_size == 0xFFFFFFFF or size == 0xFFFFFFFF or header_offset == 0xFFFFFFFF) and #extra > 0 then
            local j = 1
            while j <= #extra do
                local eid = le16(extra, j)
                local elen = le16(extra, j+2)
                if eid == 0x0001 then
                    local payload = extra:sub(j+4, j+3+elen)
                    local pidx = 1
                    if size == 0xFFFFFFFF then size = le64(payload, pidx); pidx = pidx + 8 end
                    if comp_size == 0xFFFFFFFF then comp_size = le64(payload, pidx); pidx = pidx + 8 end
                    if header_offset == 0xFFFFFFFF then header_offset = le64(payload, pidx); pidx = pidx + 8 end
                    break
                end
                j = j + 4 + elen
            end
        end
        local dos_time = le16(cd, i+12)
        local dos_date = le16(cd, i+14)
        local function dos_to_table(dt, dd)
            local sec = (dt & 0x1F) * 2
            local min = (dt >> 5) & 0x3F
            local hour = (dt >> 11) & 0x1F
            local day = dd & 0x1F
            local month = (dd >> 5) & 0x0F
            local year = ((dd >> 9) & 0x7F) + 1980
            return {year=year, month=month, day=day, hour=hour, min=min, sec=sec}
        end
        local mtime = dos_to_table(dos_time, dos_date)
        table.insert(entries, {name = name, method = method, crc = crc, comp = comp_size, size = size, header_offset = header_offset, flags = flags, mtime = mtime})
        i = i + 46 + name_len + extra_len + comment_len
    end
    f:close()
    return entries
end

-- make module exports
-- Convenience: create a zip from a directory (recursively)
-- Convenience: create a zip from a directory (recursively).
--
-- Args:
--   out_zip (string): output zip path.
--   src_dir (string): source directory to zip.
--   opts (table|nil): writer options (compress, level, force_deflate)
-- Returns:
--   true on success or (nil, err).
local function zip_dir(out_zip, src_dir, opts)
    opts = opts or {}
    if opts.compress == nil then opts.compress = true end
    if opts.level == nil then opts.level = 6 end
    local zw, err = ZipWriter.new(out_zip, opts)
    if not zw then return nil, err end
    local base = src_dir:gsub('^.*[\\/]', '')
    local ok, err2 = zw:add_tree(src_dir, base)
    if not ok then return nil, err2 end
    return zw:close()
end

-- Convenience: create a zip from a list of paths (files or directories)
-- Convenience: create a zip from a list of paths (files or directories).
--
-- Args:
--   out_zip (string): output zip path.
--   paths (table): list of filesystem paths.
--   opts (table|nil): writer options.
-- Returns:
--   true on success or (nil, err).
local function zip_paths(out_zip, paths, opts)
    if type(paths) ~= 'table' then return nil, 'paths must be a table' end
    opts = opts or {}
    if opts.compress == nil then opts.compress = true end
    if opts.level == nil then opts.level = 6 end
    local zw, err = ZipWriter.new(out_zip, opts)
    if not zw then return nil, err end
    for _, p in ipairs(paths) do
        if file_exists(p) then
            local name = p:gsub('^.*[\\/]', '')
            local ok, e = zw:add_file(p, name)
            if not ok then return nil, e end
        else
            local base = p:gsub('^.*[\\/]', '')
            local ok, e = zw:add_tree(p, base)
            if not ok then return nil, e end
        end
    end
    return zw:close()
end

M.ZipWriter = ZipWriter
M.unzip_file = unzip_file
M.deflate_lz77 = deflate_lz77
M.inflate_deflate = inflate_deflate
M.crc32 = crc32
-- optional helpers
if list_zip then M.list_zip = list_zip end
if zip_dir then M.zip_dir = zip_dir end
if zip_paths then M.zip_paths = zip_paths end

return M
