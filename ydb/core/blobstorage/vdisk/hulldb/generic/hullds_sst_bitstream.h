#pragma once

#include "defs.h"

#include <util/generic/vector.h>

// A bit stream for the packed SST index forms
namespace NKikimr {

    // Bits go MSB first: bit i of the stream is bit (63 - i % 64) of word i / 64.
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

        // the stream plus one zero word, so that ReadBits may always load the word after the current one
        TVector<ui64> Finish() && {
            Words.push_back(0);
            return std::move(Words);
        }

    private:
        TVector<ui64> Words;
        ui64 Bits = 0;
    };

    // the `bits` bits at position `pos` of a stream from TBitWriter::Finish, bits <= 64; bits past the end read as zeros
    inline ui64 ReadBits(const ui64 *words, ui64 pos, ui32 bits) {
        Y_DEBUG_ABORT_UNLESS(bits <= 64);
        if (!bits) {
            return 0;
        }
        const ui64 index = pos / 64;
        const ui32 shift = pos % 64;
        ui64 window = words[index] << shift;
        if (shift) {
            window |= words[index + 1] >> (64 - shift);
        }
        return window >> (64 - bits);
    }

    // Reads a stream from TBitWriter::Finish one value after another. Bits go MSB first, so a peeked prefix compares
    // as a plain integer (canonical Huffman codes rely on it).
    class TBitReader {
    public:
        explicit TBitReader(const ui64 *words, ui64 pos = 0)
            : Words(words)
            , Pos(pos)
        {}

        // bits past the end read as zeros
        ui64 Peek(ui32 bits) const {
            return ReadBits(Words, Pos, bits);
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

} // NKikimr
