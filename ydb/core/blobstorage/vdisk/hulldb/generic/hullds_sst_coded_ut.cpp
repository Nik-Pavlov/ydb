#include "hullds_sst_coded.h"

#include <ydb/core/blobstorage/vdisk/hulldb/base/blobstorage_blob.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullds_ut.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/random/fast.h>

namespace NKikimr {

    namespace {

        using TRec = TCodedLogoBlobIndex::TRec;

        const TBlobStorageGroupType GType(TErasureType::Erasure4Plus2Block);

        // An SST as compaction writes it: groups of steps per (tablet, channel, generation); inline blobs placed one
        // after another in data chunks, huge blobs in slots, records without data, and a few oddities.
        TTrackableVector<TRec> MakeIndex(TTestContexts& ctxs, ui32 records, ui32 seed) {
            TReallyFastRng32 rng(seed);
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            ui64 tablet = 72075186224037888ull;
            ui32 channel = 0, gen = 1, step = 1;
            ui32 dataChunk = 10, dataOffset = 0;
            ui32 manyHugeIndex = 0;
            while (index.size() < records) {
                switch (rng() % 16) {
                    case 0: tablet += 1 + rng() % 1000; channel = rng() % 3; gen = 1 + rng() % 10; step = rng() % 100; break;
                    case 1: ++channel; gen = 1 + rng() % 10; step = rng() % 100; break;
                    case 2: gen += 1 + rng() % 3; step = rng() % 100; break;
                    default: step += rng() % 4; break;
                }
                const ui32 blobSize = rng() % 4 ? 100 + rng() % 4000 : 1 + rng() % (8 << 20);
                const ui32 cookie = rng() % 8 ? 0 : rng() % 1000;
                const ui32 partId = 1 + rng() % 6;
                const TLogoBlobID id(tablet, gen, step, channel, blobSize, cookie, partId);
                if (!index.empty() && !(index.back().GetKey() < TKeyLogoBlob(id))) {
                    continue;
                }

                const ui64 ingress = (ui64(rng()) << 32 | rng()) & (rng() % 2 ? 0xFFFF000000000000ull : ~0ull);
                TMemRecLogoBlob memRec{TIngress(ingress)};
                const NMatrix::TVectorType parts = TIngress(ingress).LocalParts(GType);
                const ui32 size = TDiskBlob::CalculateBlobSize(GType, id.FullID(), parts,
                    rng() % 8 ? EBlobHeaderMode::XXH3_64BIT_HEADER : EBlobHeaderMode::OLD_HEADER)
                    + (rng() % 50 ? 0 : 1 + rng() % 100);
                switch (rng() % 10) {
                    case 0: case 1: case 2:
                        break; // no data
                    case 3: case 4: case 5: case 6:
                        if (dataOffset + size > (128 << 20) || rng() % 200 == 0) {
                            ++dataChunk;
                            dataOffset = rng() % 2 ? 0 : 4096;
                        }
                        memRec.SetDiskBlob(TDiskPart(dataChunk, dataOffset, size));
                        dataOffset += AlignUp<ui32>(size, 4);
                        break;
                    case 7: case 8:
                        memRec.SetHugeBlob(TDiskPart(1000 + rng() % 50, (rng() % 300) * (512 << 10), size));
                        break;
                    default:
                        if (rng() % 2) {
                            const ui32 num = 2 + rng() % 3;
                            memRec.SetManyHugeBlobs(manyHugeIndex, num, size);
                            manyHugeIndex += num;
                        } else {
                            memRec.SetMemBlob(ui64(rng()) << 20 | rng(), size);
                        }
                        break;
                }
                index.emplace_back(TKeyLogoBlob(id), memRec);
            }
            return index;
        }

        void CheckIndex(TTestContexts& ctxs, const TTrackableVector<TRec>& index) {
            const TCodedLogoBlobIndex coded(ctxs.GetVCtx()->SstIndex, index);
            UNIT_ASSERT_VALUES_EQUAL(coded.GetRecords(), index.size());
            UNIT_ASSERT(coded.FirstKey() == index.front().GetKey());
            UNIT_ASSERT(coded.LastKey() == index.back().GetKey());

            // decodes to the very same bytes
            TTrackableVector<TRec> saved(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            coded.SaveLinearIndex(&saved);
            UNIT_ASSERT_VALUES_EQUAL(saved.size(), index.size());
            UNIT_ASSERT(memcmp(saved.data(), index.data(), index.size() * sizeof(TRec)) == 0);

            auto expect = [&](ui64 pos, size_t i) {
                UNIT_ASSERT_VALUES_EQUAL(pos, i);
                TKeyLogoBlob key;
                TMemRecLogoBlob memRec;
                coded.Decode(pos, &key, &memRec);
                UNIT_ASSERT_VALUES_EQUAL(key.ToString(), index[i].GetKey().ToString());
                const TMemRecLogoBlob expected = index[i].GetMemRec();
                UNIT_ASSERT(memcmp(&memRec, &expected, sizeof(memRec)) == 0);
            };

            for (size_t i = index.size(); i-- > 0; ) {
                expect(i, i);
            }

            TReallyFastRng32 rng(index.size());
            for (ui32 n = 0; n < 300; ++n) {
                const size_t i = rng() % index.size();
                expect(coded.LowerBound(index[i].GetKey()), i);

                // a key right before record i lands on it too
                const TLogoBlobID id = index[i].GetKey().LogoBlobID();
                if (id.PartId() > 1 && (!i || index[i - 1].GetKey().LogoBlobID() < TLogoBlobID(id, id.PartId() - 1))) {
                    expect(coded.LowerBound(TLogoBlobID(id, id.PartId() - 1)), i);
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(coded.LowerBound(TLogoBlobID(Max<ui64>(), Max<ui32>(), Max<ui32>(), 255, 0, 0)),
                index.size());
            expect(coded.LowerBound(TLogoBlobID(0, 0, 0, 0, 0, 0)), 0);
        }

    } // anonymous

    Y_UNIT_TEST_SUITE(TCodedLogoBlobIndexTest) {

        Y_UNIT_TEST(RoundTrip) {
            TTestContexts ctxs;
            // block boundaries: one record, a block without the last record, exact blocks, a block and a record
            for (ui32 records : {1u, 63u, 64u, 65u, 128u, 3001u}) {
                CheckIndex(ctxs, MakeIndex(ctxs, records, records));
            }
        }

        Y_UNIT_TEST(SingleRecord) {
            TTestContexts ctxs;
            CheckIndex(ctxs, MakeIndex(ctxs, 1, 1));
        }

        Y_UNIT_TEST(OffsetWithoutData) {
            // a DiskBlob in chunk 0 has TypeId 0 like a record without data; its offset must survive anyway
            TTestContexts ctxs;
            auto index = MakeIndex(ctxs, 500, 7);
            TMemRecLogoBlob memRec = index[100].GetMemRec();
            memRec.SetDiskBlob(TDiskPart(0, 4096, 0));
            index[100] = TRec(index[100].GetKey(), memRec);
            CheckIndex(ctxs, index);
        }

        Y_UNIT_TEST(RecordsAreCompact) {
            TTestContexts ctxs;
            const auto index = MakeIndex(ctxs, 20000, 5);
            const TCodedLogoBlobIndex coded(ctxs.GetVCtx()->SstIndex, index);
            // below the 44 bytes of the linear record even for these random ingresses and data locations
            UNIT_ASSERT_C(coded.GetBytes() < index.size() * 24, coded.GetBytes() / double(index.size()));
        }

        Y_UNIT_TEST(MemoryIsAccounted) {
            TTestContexts ctxs;
            const auto index = MakeIndex(ctxs, 5000, 6);
            auto counter = ctxs.GetVCtx()->SstIndex.GetCounter();
            const i64 before = counter->Val();
            {
                const TCodedLogoBlobIndex coded(ctxs.GetVCtx()->SstIndex, index);
                UNIT_ASSERT_VALUES_EQUAL(counter->Val() - before, coded.GetBytes());
            }
            UNIT_ASSERT_VALUES_EQUAL(counter->Val(), before);
        }
    }

} // NKikimr
