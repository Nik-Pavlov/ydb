#include "hullds_sst_codec.h"

#include <library/cpp/testing/unittest/registar.h>

#include <util/random/fast.h>

#include <cmath>

namespace NKikimr::NSstCodec {

    namespace {

        // lengths of the code, checked to satisfy the Kraft equality (the code is complete) and the cap
        void CheckComplete(const TCodeLengths& code, ui32 maxLength) {
            UNIT_ASSERT(!code.Implied);
            double kraft = 0;
            for (ui8 len : code.Lengths) {
                UNIT_ASSERT(len <= maxLength);
                if (len) {
                    kraft += std::ldexp(1.0, -int(len));
                }
            }
            UNIT_ASSERT_DOUBLES_EQUAL(kraft, 1.0, 1e-12);
        }

        // codes `symbols` with a code built from their own frequencies, decodes them back and returns the stream size
        ui64 RoundTrip(const TVector<ui32>& symbols, ui32 alphabet, ui32 maxLength = MaxCodeLength) {
            TVector<ui64> freqs(alphabet);
            for (ui32 symbol : symbols) {
                ++freqs[symbol];
            }
            const TCodeLengths lengths = BuildCodeLengths(freqs, maxLength);
            const THuffmanEncoder encoder(lengths);
            const THuffmanDecoder decoder(lengths);

            TBitWriter writer;
            ui64 expectedBits = 0;
            for (ui32 symbol : symbols) {
                UNIT_ASSERT(lengths.Implied ? *lengths.Implied == symbol : lengths.Lengths[symbol] > 0);
                encoder.Write(writer, symbol);
                expectedBits += lengths.Implied ? 0 : lengths.Lengths[symbol];
            }
            const ui64 bits = writer.GetBits();
            UNIT_ASSERT_VALUES_EQUAL(bits, expectedBits);
            const TVector<ui64> words = std::move(writer).Finish();

            TBitReader reader(words.data());
            for (ui32 symbol : symbols) {
                UNIT_ASSERT_VALUES_EQUAL(decoder.Read(reader), symbol);
            }
            UNIT_ASSERT_VALUES_EQUAL(reader.GetPos(), bits);
            return bits;
        }

    } // anonymous

    Y_UNIT_TEST_SUITE(TSstCodecBitStream) {

        Y_UNIT_TEST(RandomWidths) {
            TReallyFastRng32 rng(1);
            TVector<std::pair<ui64, ui32>> values;
            TBitWriter writer;
            for (ui32 i = 0; i < 100000; ++i) {
                const ui32 bits = rng() % 65;
                const ui64 value = bits ? rng.GenRand64() >> (64 - bits) : 0;
                values.emplace_back(value, bits);
                writer.Write(value, bits);
            }
            const ui64 total = writer.GetBits();
            const TVector<ui64> words = std::move(writer).Finish();

            TBitReader reader(words.data());
            TVector<ui64> positions;
            for (const auto& [value, bits] : values) {
                positions.push_back(reader.GetPos());
                UNIT_ASSERT_VALUES_EQUAL(reader.Peek(bits), value);
                UNIT_ASSERT_VALUES_EQUAL(reader.Read(bits), value);
            }
            UNIT_ASSERT_VALUES_EQUAL(reader.GetPos(), total);

            // random access from any position
            for (ui32 i = 0; i < 1000; ++i) {
                const ui32 index = rng() % values.size();
                TBitReader at(words.data(), positions[index]);
                UNIT_ASSERT_VALUES_EQUAL(at.Read(values[index].second), values[index].first);
            }
        }

        Y_UNIT_TEST(HighBitsAreDropped) {
            TBitWriter writer;
            writer.Write(0xFF, 4);
            writer.Write(0, 4);
            const TVector<ui64> words = std::move(writer).Finish();
            TBitReader reader(words.data());
            UNIT_ASSERT_VALUES_EQUAL(reader.Read(8), 0xF0u);
        }

        Y_UNIT_TEST(ReadsZerosPastTheEnd) {
            TBitWriter writer;
            writer.Write(1, 1);
            const TVector<ui64> words = std::move(writer).Finish();
            TBitReader reader(words.data(), 1);
            UNIT_ASSERT_VALUES_EQUAL(reader.Peek(64), 0u);
            reader.Skip(63);
            UNIT_ASSERT_VALUES_EQUAL(reader.Peek(64), 0u);
        }
    }

    Y_UNIT_TEST_SUITE(TSstCodecHuffman) {

        Y_UNIT_TEST(KnownLengths) {
            const TCodeLengths code = BuildCodeLengths({1, 1, 2, 4, 0});
            UNIT_ASSERT_VALUES_EQUAL(code.Lengths, TVector<ui8>({3, 3, 2, 1, 0}));
            CheckComplete(code, MaxCodeLength);
        }

        Y_UNIT_TEST(CanonicalCodes) {
            // lengths 2, 1, 3, 3 give the canonical codes 10, 0, 110, 111
            TCodeLengths code;
            code.Lengths = {2, 1, 3, 3};
            const THuffmanEncoder encoder(code);
            TBitWriter writer;
            for (ui32 symbol : {0, 1, 2, 3}) {
                encoder.Write(writer, symbol);
            }
            const TVector<ui64> words = std::move(writer).Finish();
            TBitReader reader(words.data());
            UNIT_ASSERT_VALUES_EQUAL(reader.Read(9), 0b10'0'110'111u);
        }

        Y_UNIT_TEST(NoSymbols) {
            const TCodeLengths code = BuildCodeLengths({0, 0, 0});
            UNIT_ASSERT(!code.Implied);
            UNIT_ASSERT_VALUES_EQUAL(code.Lengths, TVector<ui8>({0, 0, 0}));
        }

        Y_UNIT_TEST(ImpliedSymbolCostsNothing) {
            const TVector<ui32> symbols(1000, 7);
            UNIT_ASSERT_VALUES_EQUAL(RoundTrip(symbols, 10), 0u);

            const TCodeLengths code = BuildCodeLengths({0, 0, 5});
            UNIT_ASSERT_VALUES_EQUAL(*code.Implied, 2u);
        }

        Y_UNIT_TEST(TwoSymbols) {
            UNIT_ASSERT_VALUES_EQUAL(RoundTrip({0, 1, 1, 1, 0}, 2), 5u);
        }

        Y_UNIT_TEST(RandomSkewed) {
            TReallyFastRng32 rng(2);
            for (ui32 alphabet : {3, 17, 300, 5000}) {
                TVector<ui32> symbols;
                TVector<ui64> freqs(alphabet);
                for (ui32 i = 0; i < 200000; ++i) {
                    // geometric-like: small symbols dominate
                    const ui32 symbol = Min<ui32>(alphabet - 1, std::countr_zero(rng() | (1u << 31)) * (alphabet / 16 + 1)
                        + rng() % (alphabet / 16 + 1));
                    symbols.push_back(symbol);
                    ++freqs[symbol];
                }
                const ui64 bits = RoundTrip(symbols, alphabet);

                // Huffman is within 1 bit per symbol of the entropy
                double entropy = 0;
                for (ui64 f : freqs) {
                    if (f) {
                        entropy -= f * std::log2(double(f) / symbols.size());
                    }
                }
                UNIT_ASSERT(bits >= entropy);
                UNIT_ASSERT(bits < entropy + symbols.size());
                CheckComplete(BuildCodeLengths(freqs), MaxCodeLength);
            }
        }

        Y_UNIT_TEST(LengthCap) {
            // Fibonacci frequencies make the Huffman tree as deep as the alphabet is large
            TVector<ui64> freqs{1, 1};
            while (freqs.size() < 40) {
                freqs.push_back(freqs[freqs.size() - 1] + freqs[freqs.size() - 2]);
            }
            for (ui32 cap : {MaxCodeLength, 8u, 6u}) {
                const TCodeLengths code = BuildCodeLengths(freqs, cap);
                CheckComplete(code, cap);
                // more frequent symbols never get longer codes
                for (ui32 i = 1; i < freqs.size(); ++i) {
                    UNIT_ASSERT(code.Lengths[i] <= code.Lengths[i - 1]);
                }
            }
            TVector<ui32> symbols;
            for (ui32 symbol = 0; symbol < freqs.size(); ++symbol) {
                for (ui64 i = 0; i < Min<ui64>(freqs[symbol], 1000); ++i) {
                    symbols.push_back(symbol);
                }
            }
            RoundTrip(symbols, freqs.size());
            RoundTrip(symbols, freqs.size(), 6);
        }

        Y_UNIT_TEST(FullAlphabet) {
            // as many symbols as the cap allows: all codes are MaxCodeLength bits long
            const ui32 alphabet = 1u << MaxCodeLength;
            TVector<ui32> symbols;
            for (ui32 symbol = 0; symbol < alphabet; ++symbol) {
                symbols.push_back(symbol);
            }
            UNIT_ASSERT_VALUES_EQUAL(RoundTrip(symbols, alphabet), ui64(alphabet) * MaxCodeLength);
        }
    }

    Y_UNIT_TEST_SUITE(TSstCodecIntegers) {

        TVector<ui64> SampleValues() {
            TVector<ui64> values{0, 1, 2, 3, 4, 255, 256, 65535, 65536, Max<ui32>(), ui64(1) << 40, (ui64(1) << 62) - 1};
            TReallyFastRng32 rng(3);
            for (ui32 i = 0; i < 10000; ++i) {
                values.push_back(rng.GenRand64() >> (2 + rng() % 62));
            }
            return values;
        }

        Y_UNIT_TEST(Bucketed) {
            const TVector<ui64> values = SampleValues();
            TVector<ui64> freqs(65);
            for (ui64 value : values) {
                ++freqs[BitWidth(value)];
            }
            const TCodeLengths widths = BuildCodeLengths(freqs);
            const THuffmanEncoder encoder(widths);
            const THuffmanDecoder decoder(widths);

            TBitWriter writer;
            for (ui64 value : values) {
                WriteBucketed(writer, encoder, value);
            }
            const TVector<ui64> words = std::move(writer).Finish();
            TBitReader reader(words.data());
            for (ui64 value : values) {
                UNIT_ASSERT_VALUES_EQUAL(ReadBucketed(reader, decoder), value);
            }
        }

        Y_UNIT_TEST(BucketedFullRange) {
            TVector<ui64> freqs(65, 1);
            const TCodeLengths widths = BuildCodeLengths(freqs);
            const THuffmanEncoder encoder(widths);
            const THuffmanDecoder decoder(widths);
            TBitWriter writer;
            for (ui64 value : {ui64(0), ui64(1), Max<ui64>(), ui64(1) << 63}) {
                WriteBucketed(writer, encoder, value);
            }
            const TVector<ui64> words = std::move(writer).Finish();
            TBitReader reader(words.data());
            for (ui64 value : {ui64(0), ui64(1), Max<ui64>(), ui64(1) << 63}) {
                UNIT_ASSERT_VALUES_EQUAL(ReadBucketed(reader, decoder), value);
            }
        }
    }

} // NKikimr::NSstCodec
