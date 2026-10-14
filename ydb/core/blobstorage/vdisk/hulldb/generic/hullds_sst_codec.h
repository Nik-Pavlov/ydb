#pragma once

#include "defs.h"

#include <util/generic/hash.h>
#include <util/generic/vector.h>

#include <array>
#include <bit>
#include <optional>

// Entropy coding primitives for the coded SST index: a bit stream, canonical Huffman codes with a length cap, and
// a code for integers (bit-width buckets).
namespace NKikimr::NSstCodec {

    // Bits go MSB first: bit i of the stream is bit (63 - i % 64) of word i / 64. Peeked canonical Huffman codes then
    // compare as plain integers.
    class TBitWriter {
    public:
        // writes the low `bits` bits of `value`, bits <= 64
        void Write(ui64 value, ui32 bits) {
            Y_DEBUG_ABORT_UNLESS(bits <= 64);
            if (!bits) {
                return;
            }
            if (bits < 64) {
                value &= (ui64(1) << bits) - 1;
            }
            const ui32 used = Bits % 64;
            if (!used) {
                Words.push_back(0);
            }
            const ui32 free = 64 - used;
            if (bits <= free) {
                Words.back() |= value << (free - bits);
            } else {
                Words.back() |= value >> (bits - free);
                Words.push_back(value << (64 - (bits - free)));
            }
            Bits += bits;
        }

        ui64 GetBits() const {
            return Bits;
        }

        // the stream plus one zero word, so that TBitReader may always load the word after the current one
        TVector<ui64> Finish() && {
            Words.push_back(0);
            return std::move(Words);
        }

    private:
        TVector<ui64> Words;
        ui64 Bits = 0;
    };

    class TBitReader {
    public:
        // `words` must come from TBitWriter::Finish
        explicit TBitReader(const ui64 *words, ui64 pos = 0)
            : Words(words)
            , Pos(pos)
        {}

        // the next 64 bits of the stream, MSB first; bits past the end read as zeros
        ui64 Window() const {
            const ui64 index = Pos / 64;
            const ui32 shift = Pos % 64;
            ui64 res = Words[index] << shift;
            if (shift) {
                res |= Words[index + 1] >> (64 - shift);
            }
            return res;
        }

        ui64 Peek(ui32 bits) const {
            Y_DEBUG_ABORT_UNLESS(bits <= 64);
            return bits ? Window() >> (64 - bits) : 0;
        }

        void Skip(ui32 bits) {
            Pos += bits;
        }

        ui64 Read(ui32 bits) {
            const ui64 res = Peek(bits);
            Skip(bits);
            return res;
        }

        ui64 GetPos() const {
            return Pos;
        }

    private:
        const ui64 *Words;
        ui64 Pos;
    };

    // Longest Huffman code. A decoder peeks this many bits at a time.
    static constexpr ui32 MaxCodeLength = 16;

    struct TCodeLengths {
        // code length per symbol; 0 means the symbol has no code
        TVector<ui8> Lengths;
        // set when only one symbol is used: it is implied and coded with no bits at all
        std::optional<ui32> Implied;
    };

    // Huffman code lengths for symbols 0..freqs.size()-1, none longer than maxLength. Symbols with zero frequency get
    // no code. At most 2^maxLength symbols may be used.
    TCodeLengths BuildCodeLengths(const TVector<ui64>& freqs, ui32 maxLength = MaxCodeLength);

    // Canonical codes: within a length, codes grow with the symbol; shorter codes precede longer ones.
    class THuffmanEncoder {
    public:
        explicit THuffmanEncoder(const TCodeLengths& lengths);

        void Write(TBitWriter& writer, ui32 symbol) const {
            if (Implied) {
                Y_DEBUG_ABORT_UNLESS(symbol == *Implied);
                return;
            }
            Y_DEBUG_ABORT_UNLESS(symbol < Lengths.size() && Lengths[symbol]);
            writer.Write(Codes[symbol], Lengths[symbol]);
        }

    private:
        TVector<ui16> Codes;
        TVector<ui8> Lengths;
        std::optional<ui32> Implied;
    };

    class THuffmanDecoder {
    public:
        explicit THuffmanDecoder(const TCodeLengths& lengths);

        ui32 Read(TBitReader& reader) const {
            if (Implied) {
                return *Implied;
            }
            const ui32 window = reader.Peek(MaxCodeLength);
            for (ui32 len = MinLength; len <= MaxLength; ++len) {
                if (window < Limit[len]) {
                    reader.Skip(len);
                    return Symbols[Offset[len] + (window >> (MaxCodeLength - len)) - First[len]];
                }
            }
            Y_ABORT("corrupt Huffman code");
        }

        // memory the decoder keeps, without the object itself
        size_t GetAllocatedBytes() const {
            return Symbols.capacity() * sizeof(ui32);
        }

    private:
        // codes of length `len` are First[len] .. First[len] + count - 1; aligned to MaxCodeLength bits, every one of
        // them is below Limit[len], and every longer code is not
        std::array<ui32, MaxCodeLength + 1> First{};
        std::array<ui32, MaxCodeLength + 1> Limit{};
        // index in Symbols of the first code of length `len`
        std::array<ui32, MaxCodeLength + 1> Offset{};
        ui32 MinLength = 0;
        ui32 MaxLength = 0;
        // symbols in code order
        TVector<ui32> Symbols;
        std::optional<ui32> Implied;
    };

    inline ui32 BitWidth(ui64 value) {
        return 64 - std::countl_zero(value);
    }

    // A value is coded as its bit width (0..64, a Huffman symbol) followed by the bits below its top bit. Suits values
    // spread over a wide range, e.g. deltas: the table has at most 65 symbols.
    inline void WriteBucketed(TBitWriter& writer, const THuffmanEncoder& widths, ui64 value) {
        const ui32 width = BitWidth(value);
        widths.Write(writer, width);
        if (width > 1) {
            writer.Write(value, width - 1);
        }
    }

    inline ui64 ReadBucketed(TBitReader& reader, const THuffmanDecoder& widths) {
        const ui32 width = widths.Read(reader);
        Y_ABORT_UNLESS(width <= 64, "corrupt bucketed value");
        return width > 1 ? (ui64(1) << (width - 1)) | reader.Read(width - 1) : width;
    }

    // The code for the values of one field: values frequent enough to pay for their table entry get Huffman symbols,
    // the rest share an escape symbol followed by the value as a bucketed integer. TValueEncoder chooses the palette
    // and writes values; TValueCode keeps only what reading them needs.
    class TValueCode {
    public:
        // codes nothing
        TValueCode() = default;

        // `palette` maps symbols to values; symbol palette.size() is the escape
        TValueCode(TVector<ui64> palette, const TCodeLengths& symbols, const TCodeLengths& escapeWidths)
            : Palette(std::move(palette))
            , Symbols(symbols)
            , EscapeWidths(escapeWidths)
        {
            Palette.shrink_to_fit();
        }

        ui64 Read(TBitReader& reader) const {
            const ui32 symbol = Symbols.Read(reader);
            return symbol < Palette.size() ? Palette[symbol] : ReadBucketed(reader, EscapeWidths);
        }

        // memory the code takes, including the object itself
        size_t GetBytes() const {
            return sizeof(*this) + Palette.capacity() * sizeof(ui64) + Symbols.GetAllocatedBytes()
                + EscapeWidths.GetAllocatedBytes();
        }

    private:
        TVector<ui64> Palette;
        THuffmanDecoder Symbols{TCodeLengths{}};
        THuffmanDecoder EscapeWidths{TCodeLengths{}};
    };

    // Writes the values of one field, with a code built from the values the field takes. Only values seen at
    // construction can be written. Needed only while writing; GetCode() gives the code to read them back.
    class TValueEncoder {
    public:
        // `freqs` maps the values of the field to their counts
        explicit TValueEncoder(const THashMap<ui64, ui64>& freqs);

        void Write(TBitWriter& writer, ui64 value) const {
            if (const auto it = SymbolOf.find(value); it != SymbolOf.end()) {
                Symbols.Write(writer, it->second);
            } else {
                Symbols.Write(writer, Palette.size());
                WriteBucketed(writer, EscapeWidths, value);
            }
        }

        TValueCode GetCode() const {
            return TValueCode(Palette, SymbolLengths, EscapeWidthLengths);
        }

    private:
        TVector<ui64> Palette;
        THashMap<ui64, ui32> SymbolOf;
        TCodeLengths SymbolLengths;
        TCodeLengths EscapeWidthLengths;
        THuffmanEncoder Symbols{TCodeLengths{}};
        THuffmanEncoder EscapeWidths{TCodeLengths{}};
    };

} // NKikimr::NSstCodec
