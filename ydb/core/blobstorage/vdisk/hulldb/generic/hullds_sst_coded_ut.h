#pragma once

#include <ydb/core/blobstorage/vdisk/hulldb/base/blobstorage_blob.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullds_ut.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/random/fast.h>

// Test data and checks for the coded LogoBlob SST index form
namespace NKikimr::NSstIndexTest {

    namespace {

        using TRec = TIndexRecord<TKeyLogoBlob, TMemRecLogoBlob>;

        const TBlobStorageGroupType GType(TErasureType::Erasure4Plus2Block);

        // An SST as compaction writes it: groups of steps per (tablet, channel, generation); inline blobs placed one
        // after another in data chunks, huge blobs in slots, records without data, and a few oddities. A record
        // starts a new group with probability 3 / groupSwitch: about 5 records per group by default, the production
        // average of about 500 at groupSwitch = 1500. Ingresses are random 64-bit values, or drawn from
        // `ingressValues` random ones when that is set (production SSTs see a handful of distinct ingresses).
        TTrackableVector<TRec> MakeIndex(TTestContexts& ctxs, ui32 records, ui32 seed, ui32 groupSwitch = 16,
                ui32 ingressValues = 0) {
            TReallyFastRng32 rng(seed);
            TVector<ui64> ingresses;
            for (ui32 i = 0; i < ingressValues; ++i) {
                ingresses.push_back(ui64(rng()) << 32 | rng());
            }
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            ui64 tablet = 72075186224037888ull;
            ui32 channel = 0, gen = 1, step = 1;
            ui32 dataChunk = 10, dataOffset = 0;
            ui32 manyHugeIndex = 0;
            while (index.size() < records) {
                switch (rng() % groupSwitch) {
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

                const ui64 ingress = ingressValues ? ingresses[rng() % ingressValues]
                    : (ui64(rng()) << 32 | rng()) & (rng() % 2 ? 0xFFFF000000000000ull : ~0ull);
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

        template <class TIterator>
        void ExpectAt(const TIterator& it, const TTrackableVector<TRec>& index, size_t i) {
            UNIT_ASSERT(it.Valid());
            UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), index[i].GetKey().ToString());
            const TMemRecLogoBlob memRec = it.GetMemRec();
            const TMemRecLogoBlob expected = index[i].GetMemRec();
            UNIT_ASSERT(memcmp(&memRec, &expected, sizeof(memRec)) == 0);
        }

        // builds the index form TIndex from `index` and checks Save, walks in both directions and seeks
        template <class TIndex>
        void CheckIndex(TTestContexts& ctxs, const TTrackableVector<TRec>& index) {
            const TIndex form(ctxs.GetVCtx()->SstIndex, index);
            UNIT_ASSERT_VALUES_EQUAL(form.Elements(), index.size());
            UNIT_ASSERT(form.FirstKey() == index.front().GetKey());
            UNIT_ASSERT(form.LastKey() == index.back().GetKey());

            // decodes to the very same bytes
            TTrackableVector<TRec> saved(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            form.Save(&saved);
            UNIT_ASSERT_VALUES_EQUAL(saved.size(), index.size());
            UNIT_ASSERT(memcmp(saved.data(), index.data(), index.size() * sizeof(TRec)) == 0);

            // forward and backward walks
            typename TIndex::TIterator it(&form);
            it.SeekToFirst();
            for (size_t i = 0; i < index.size(); ++i, it.Next()) {
                ExpectAt(it, index, i);
            }
            UNIT_ASSERT(!it.Valid());
            it.Prev();
            for (size_t i = index.size(); i-- > 0; it.Prev()) {
                ExpectAt(it, index, i);
            }
            UNIT_ASSERT(!it.Valid());
            it.SeekToLast();
            ExpectAt(it, index, index.size() - 1);

            // seeks: every key, a key right before it (lands on it), a key right after it (lands on the next)
            for (size_t i = 0; i < index.size(); ++i) {
                const TLogoBlobID id = index[i].GetKey().LogoBlobID();
                it.Seek(index[i].GetKey());
                ExpectAt(it, index, i);

                if (id.PartId() > 1 && (!i || index[i - 1].GetKey().LogoBlobID() < TLogoBlobID(id, id.PartId() - 1))) {
                    it.Seek(TLogoBlobID(id, id.PartId() - 1));
                    ExpectAt(it, index, i);
                }
                if (id.PartId() < TLogoBlobID::MaxPartId) {
                    it.Seek(TLogoBlobID(id, id.PartId() + 1));
                    if (i + 1 < index.size()) {
                        ExpectAt(it, index, i + 1);
                    } else {
                        UNIT_ASSERT(!it.Valid());
                    }
                }
            }
            it.Seek(TLogoBlobID(Max<ui64>(), Max<ui32>(), Max<ui32>(), 255, 0, 0));
            UNIT_ASSERT(!it.Valid());
            it.Prev();
            ExpectAt(it, index, index.size() - 1);
            it.Seek(TLogoBlobID(0, 0, 0, 0, 0, 0));
            ExpectAt(it, index, 0);
        }

        // records of one group with the given non-decreasing steps
        TTrackableVector<TRec> MakeGroupIndex(TTestContexts& ctxs, const TVector<ui32>& steps, ui32 tablet = 7) {
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            for (ui32 i = 0; i < steps.size(); ++i) {
                TMemRecLogoBlob memRec;
                memRec.SetDiskBlob(TDiskPart(1, i * 4096, 100 + i));
                index.emplace_back(TKeyLogoBlob(TLogoBlobID(tablet, 1, steps[i], 0, 100 + i, i, 1 + i % 5)), memRec);
            }
            return index;
        }

        // The checks below run against every form.

        template <class TIndex>
        void CheckRoundTrips(TTestContexts& ctxs) {
            // block and frame boundaries: one record, a block without the last record, exact blocks, a block and
            // a record; short groups, and groups of production length that span blocks
            for (ui32 records : {1u, 31u, 32u, 33u, 63u, 64u, 65u, 128u, 3001u}) {
                CheckIndex<TIndex>(ctxs, MakeIndex(ctxs, records, records));
                CheckIndex<TIndex>(ctxs, MakeIndex(ctxs, records, records, 1500));
            }
        }

        template <class TIndex>
        void CheckEveryRecordItsOwnGroup(TTestContexts& ctxs) {
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            for (ui32 i = 0; i < 100; ++i) {
                index.emplace_back(TKeyLogoBlob(TLogoBlobID(1000 + i, 1, 1, 0, 10, 0)), TMemRecLogoBlob());
            }
            CheckIndex<TIndex>(ctxs, index);
        }

        template <class TIndex>
        void CheckStepJumpsAndRepeats(TTestContexts& ctxs) {
            // a frame boundary between equal steps, a wide step jump at a frame start, steps up to the maximum
            TVector<ui32> steps;
            for (ui32 i = 0; i < 70; ++i) {
                steps.push_back(i < 40 ? 5 : i < 64 ? 6 : 5 + (i - 63) * (Max<ui32>() / 8));
            }
            CheckIndex<TIndex>(ctxs, MakeGroupIndex(ctxs, steps));
        }

        template <class TIndex>
        void CheckSeekBetweenGroups(TTestContexts& ctxs) {
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            for (ui32 tablet : {10u, 20u, 30u}) {
                for (ui32 step = 1; step <= 50; ++step) {
                    index.emplace_back(TKeyLogoBlob(TLogoBlobID(tablet, 1, step, 0, 10, 0)), TMemRecLogoBlob());
                }
            }
            const TIndex form(ctxs.GetVCtx()->SstIndex, index);
            typename TIndex::TIterator it(&form);
            // a tablet between two groups: the next group's first record
            it.Seek(TLogoBlobID(15, 1, 1, 0, 10, 0));
            ExpectAt(it, index, 50);
            // a step above the last of a group: the next group's first record
            it.Seek(TLogoBlobID(20, 1, 51, 0, 10, 0));
            ExpectAt(it, index, 100);
            // a step above the last group: past the end
            it.Seek(TLogoBlobID(30, 1, 51, 0, 10, 0));
            UNIT_ASSERT(!it.Valid());
            // a step below the first of a group: that group's first record
            it.Seek(TLogoBlobID(30, 1, 0, 0, 10, 0));
            ExpectAt(it, index, 100);
            CheckIndex<TIndex>(ctxs, index);
        }

        template <class TIndex>
        void CheckOffsetWithoutData(TTestContexts& ctxs) {
            // a DiskBlob in chunk 0 has TypeId 0 like a record without data; its offset must survive anyway
            auto index = MakeIndex(ctxs, 500, 7);
            TMemRecLogoBlob memRec = index[100].GetMemRec();
            memRec.SetDiskBlob(TDiskPart(0, 4096, 0));
            index[100] = TRec(index[100].GetKey(), memRec);
            CheckIndex<TIndex>(ctxs, index);
        }

    } // anonymous

} // NKikimr::NSstIndexTest
