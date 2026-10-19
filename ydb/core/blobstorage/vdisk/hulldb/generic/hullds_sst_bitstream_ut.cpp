#include "hullds_sst_bitstream.h"

#include <library/cpp/testing/unittest/registar.h>

#include <util/random/fast.h>

namespace NKikimr {

    Y_UNIT_TEST_SUITE(TSstCodecBitStream) {

        Y_UNIT_TEST(RandomWidths) {
            TReallyFastRng32 rng(1);
            TVector<std::pair<ui64, ui32>> values;
            TVector<ui64> positions;
            TBitWriter writer;
            for (ui32 i = 0; i < 100000; ++i) {
                const ui32 bits = rng() % 65;
                const ui64 value = bits ? rng.GenRand64() >> (64 - bits) : 0;
                values.emplace_back(value, bits);
                positions.push_back(writer.GetBits());
                writer.Write(value, bits);
            }
            const TVector<ui64> words = std::move(writer).Finish();

            for (size_t i = 0; i < values.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(ReadBits(words.data(), positions[i], values[i].second), values[i].first);
            }
        }

        Y_UNIT_TEST(HighBitsAreDropped) {
            TBitWriter writer;
            writer.Write(0xFF, 4);
            writer.Write(0, 4);
            const TVector<ui64> words = std::move(writer).Finish();
            UNIT_ASSERT_VALUES_EQUAL(ReadBits(words.data(), 0, 8), 0xF0u);
        }

        Y_UNIT_TEST(ReadsZerosPastTheEnd) {
            TBitWriter writer;
            writer.Write(1, 1);
            const TVector<ui64> words = std::move(writer).Finish();
            UNIT_ASSERT_VALUES_EQUAL(ReadBits(words.data(), 1, 64), 0u);
            UNIT_ASSERT_VALUES_EQUAL(ReadBits(words.data(), 64, 64), 0u);
        }
    }

} // NKikimr
