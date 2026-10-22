#pragma once

#include "hullds_sst_bitstream.h"

#include <util/generic/hash.h>
#include <util/generic/vector.h>

#include <array>
#include <bit>
#include <optional>

// Entropy coding primitives for the coded SST index: canonical Huffman codes with a length cap, and a code for
// integers (bit-width buckets).
namespace NKikimr::NSstCodec {

    inline ui32 BitWidth(ui64 value) {
        return std::bit_width(value);
    }

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
