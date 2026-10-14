#include "hullds_sst_codec.h"

#include <util/generic/algorithm.h>

#include <cmath>
#include <queue>

namespace NKikimr::NSstCodec {

    namespace {

        using TLengthCounts = std::array<ui32, MaxCodeLength + 1>;

        TLengthCounts CountLengths(const TVector<ui8>& lengths) {
            TLengthCounts count{};
            for (ui8 len : lengths) {
                Y_ABORT_UNLESS(len <= MaxCodeLength);
                ++count[len];
            }
            count[0] = 0;
            return count;
        }

        // the first canonical code of every length
        TLengthCounts FirstCodes(const TLengthCounts& count) {
            TLengthCounts first{};
            ui32 code = 0;
            for (ui32 len = 1; len <= MaxCodeLength; ++len) {
                code = (code + count[len - 1]) << 1;
                Y_ABORT_UNLESS(code + count[len] <= (1u << len), "oversubscribed Huffman code lengths");
                first[len] = code;
            }
            return first;
        }

    } // anonymous

    TCodeLengths BuildCodeLengths(const TVector<ui64>& freqs, ui32 maxLength) {
        Y_ABORT_UNLESS(maxLength >= 1 && maxLength <= MaxCodeLength);

        TCodeLengths res;
        res.Lengths.assign(freqs.size(), 0);

        TVector<ui32> used;
        for (ui32 symbol = 0; symbol < freqs.size(); ++symbol) {
            if (freqs[symbol]) {
                used.push_back(symbol);
            }
        }
        if (used.size() <= 1) {
            if (!used.empty()) {
                res.Implied = used.front();
            }
            return res;
        }
        Y_ABORT_UNLESS(used.size() <= (ui64(1) << maxLength), "too many symbols for the code length cap");

        // most frequent first; ties go by symbol, so that the result is deterministic
        Sort(used, [&](ui32 x, ui32 y) {
            return freqs[x] != freqs[y] ? freqs[x] > freqs[y] : x < y;
        });

        // Huffman tree: leaves are 0..n-1 in `used` order, internal nodes follow in the order they are made, so every
        // parent has a larger index than its children and the root is the last node
        const ui32 n = used.size();
        TVector<ui32> parent(2 * n - 1);
        using TItem = std::pair<ui64, ui32>; // weight, node
        std::priority_queue<TItem, TVector<TItem>, std::greater<TItem>> heap;
        for (ui32 i = 0; i < n; ++i) {
            heap.emplace(freqs[used[i]], i);
        }
        for (ui32 node = n; heap.size() > 1; ++node) {
            const auto [weightX, x] = heap.top();
            heap.pop();
            const auto [weightY, y] = heap.top();
            heap.pop();
            parent[x] = parent[y] = node;
            heap.emplace(weightX + weightY, node);
        }
        TVector<ui32> depth(2 * n - 1);
        for (ui32 node = 2 * n - 2; node-- > 0; ) {
            depth[node] = depth[parent[node]] + 1;
        }

        // Cap the lengths, then restore the Kraft equality: drop one code of the longest length and split a shorter
        // code in two, until the code is complete again. Lengths then go to symbols by frequency.
        std::array<ui32, MaxCodeLength + 1> count{};
        for (ui32 leaf = 0; leaf < n; ++leaf) {
            ++count[Min(depth[leaf], maxLength)];
        }
        ui64 total = 0;
        for (ui32 len = 1; len <= maxLength; ++len) {
            total += ui64(count[len]) << (maxLength - len);
        }
        while (total != (ui64(1) << maxLength)) {
            Y_ABORT_UNLESS(total > (ui64(1) << maxLength));
            --count[maxLength];
            for (ui32 len = maxLength - 1; len > 0; --len) {
                if (count[len]) {
                    --count[len];
                    count[len + 1] += 2;
                    break;
                }
            }
            --total;
        }

        auto symbol = used.begin();
        for (ui32 len = 1; len <= maxLength; ++len) {
            for (ui32 i = 0; i < count[len]; ++i) {
                res.Lengths[*symbol++] = len;
            }
        }
        Y_ABORT_UNLESS(symbol == used.end());
        return res;
    }

    THuffmanEncoder::THuffmanEncoder(const TCodeLengths& lengths)
        : Lengths(lengths.Lengths)
        , Implied(lengths.Implied)
    {
        TLengthCounts next = FirstCodes(CountLengths(Lengths));
        Codes.assign(Lengths.size(), 0);
        for (ui32 symbol = 0; symbol < Lengths.size(); ++symbol) {
            if (const ui8 len = Lengths[symbol]) {
                Codes[symbol] = next[len]++;
            }
        }
    }

    THuffmanDecoder::THuffmanDecoder(const TCodeLengths& lengths)
        : Implied(lengths.Implied)
    {
        const TLengthCounts count = CountLengths(lengths.Lengths);
        First = FirstCodes(count);

        MinLength = MaxCodeLength + 1;
        ui32 offset = 0;
        for (ui32 len = 1; len <= MaxCodeLength; ++len) {
            Offset[len] = offset;
            offset += count[len];
            Limit[len] = (First[len] + count[len]) << (MaxCodeLength - len);
            if (count[len]) {
                MinLength = Min(MinLength, len);
                MaxLength = len;
            }
        }

        Symbols.resize(offset);
        TLengthCounts pos = Offset;
        for (ui32 symbol = 0; symbol < lengths.Lengths.size(); ++symbol) {
            if (const ui8 len = lengths.Lengths[symbol]) {
                Symbols[pos[len]++] = symbol;
            }
        }
    }

    TValueEncoder::TValueEncoder(const THashMap<ui64, ui64>& freqs) {
        ui64 total = 0;
        for (const auto& [value, freq] : freqs) {
            total += freq;
        }

        // A value gets its own symbol when that saves more stream bits than its table entry (value + decoder slot)
        // costs. The escape costs about its symbol plus the width code, estimated at 4 bits each.
        static constexpr double EntryBits = 8 * (sizeof(ui64) + sizeof(ui32));
        static constexpr ui32 MaxPalette = (1u << MaxCodeLength) - 1;
        TVector<std::pair<ui64, ui64>> candidates; // freq, value
        for (const auto& [value, freq] : freqs) {
            const double symbolBits = std::log2(double(total) / freq);
            const double escapeBits = 8 + (BitWidth(value) > 1 ? BitWidth(value) - 1 : 0);
            if (freq * (escapeBits - symbolBits) > EntryBits) {
                candidates.emplace_back(freq, value);
            }
        }
        Sort(candidates, [](const auto& x, const auto& y) {
            return x.first != y.first ? x.first > y.first : x.second < y.second;
        });
        if (candidates.size() > MaxPalette) {
            candidates.resize(MaxPalette);
        }

        TVector<ui64> symbolFreqs;
        ui64 palettized = 0;
        for (const auto& [freq, value] : candidates) {
            SymbolOf.emplace(value, Palette.size());
            Palette.push_back(value);
            symbolFreqs.push_back(freq);
            palettized += freq;
        }
        symbolFreqs.push_back(total - palettized); // the escape

        TVector<ui64> widthFreqs(65);
        for (const auto& [value, freq] : freqs) {
            if (!SymbolOf.contains(value)) {
                widthFreqs[BitWidth(value)] += freq;
            }
        }

        SymbolLengths = BuildCodeLengths(symbolFreqs);
        EscapeWidthLengths = BuildCodeLengths(widthFreqs);
        Symbols = THuffmanEncoder(SymbolLengths);
        EscapeWidths = THuffmanEncoder(EscapeWidthLengths);
    }

} // NKikimr::NSstCodec
