#include "hullds_sst_it_all_ut.h"
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_block.h>

namespace NKikimr {

    Y_UNIT_TEST_SUITE(TBlobStorageHullSstIt) {

        using namespace NBlobStorageHullSstItHelpers;
        using TMemIterator = TLogoBlobSst::TMemIterator;

        Y_UNIT_TEST(TestSeekToFirst) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());
            it.SeekToFirst();

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Next();
            }
            TString result("[0:0:10:0:0:0:0][0:0:11:0:0:0:0]"
                          "[0:0:12:0:0:0:0][0:0:13:0:0:0:0]"
                          "[0:0:14:0:0:0:0][0:0:15:0:0:0:0]"
                          "[0:0:16:0:0:0:0][0:0:17:0:0:0:0]"
                          "[0:0:18:0:0:0:0][0:0:19:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekToLast) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());
            it.SeekToLast();

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Prev();
            }
            TString result("[0:0:19:0:0:0:0][0:0:18:0:0:0:0]"
                          "[0:0:17:0:0:0:0][0:0:16:0:0:0:0]"
                          "[0:0:15:0:0:0:0][0:0:14:0:0:0:0]"
                          "[0:0:13:0:0:0:0][0:0:12:0:0:0:0]"
                          "[0:0:11:0:0:0:0][0:0:10:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekExactAndNext) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 15, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(it.GetCurKey().ToString() == TString("[0:0:15:0:0:0:0]"));

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Next();
            }
            TString result("[0:0:15:0:0:0:0][0:0:16:0:0:0:0]"
                          "[0:0:17:0:0:0:0][0:0:18:0:0:0:0]"
                          "[0:0:19:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekExactAndPrev) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 15, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(it.GetCurKey().ToString() == TString("[0:0:15:0:0:0:0]"));

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Prev();
            }
            TString result("[0:0:15:0:0:0:0][0:0:14:0:0:0:0]"
                          "[0:0:13:0:0:0:0][0:0:12:0:0:0:0]"
                          "[0:0:11:0:0:0:0][0:0:10:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekBefore) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 5, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(it.GetCurKey().ToString() == "[0:0:10:0:0:0:0]");
        }

        Y_UNIT_TEST(TestSeekAfterAndPrev) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 1));
            TMemIterator it(ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 25, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(!it.Valid());
            it.Prev();
            UNIT_ASSERT(it.Valid());
            UNIT_ASSERT(it.GetCurKey().ToString() == "[0:0:19:0:0:0:0]");
        }

        Y_UNIT_TEST(TestSeekNotExactBefore) {
            TLogoBlobSstPtr ptr(GenerateSst(10, 10, 2));
            TMemIterator it(ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 15, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(it.GetCurKey().ToString() == "[0:0:16:0:0:0:0]");
        }

        Y_UNIT_TEST(TestSstIndexSeekAndIterate) {
            TTestContexts ctxs;
            TTrackableVector<TLogoBlobSst::TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));

            auto addRecord = [&index](ui64 tabletId, ui32 step) {
                TLogoBlobID id(tabletId, 0, step, 0, 0, 0);
                index.emplace_back(TKeyLogoBlob(id), TMemRecLogoBlob());
            };

            addRecord(10, 0);
            addRecord(10, 10);
            addRecord(20, 0);
            addRecord(20, 10);
            addRecord(20, 300);

            TLogoBlobSstPtr ptr(new TLogoBlobSst(ctxs.GetVCtx()));
            ptr->LoadLinearIndex(std::move(index));

            TMemIterator it(ptr.Get());

            it.Seek(TLogoBlobID(5, 0, 0, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[10:0:0:0:0:0:0]");

            it.Seek(TLogoBlobID(10, 0, 0, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[10:0:0:0:0:0:0]");

            it.Seek(TLogoBlobID(10, 0, 5, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[10:0:10:0:0:0:0]");

            it.Seek(TLogoBlobID(10, 0, 10, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[10:0:10:0:0:0:0]");

            it.Seek(TLogoBlobID(10, 0, 15, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:0:0:0:0:0]");

            it.Seek(TLogoBlobID(15, 0, 0, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:0:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 0, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:0:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 5, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:10:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 10, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:10:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 15, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:300:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 300, 0, 0, 0));
            UNIT_ASSERT(it.GetCurKey().ToString() == "[20:0:300:0:0:0:0]");

            it.Seek(TLogoBlobID(20, 0, 400, 0, 0, 0));
            UNIT_ASSERT(!it.Valid());

            it.Seek(TLogoBlobID(25, 0, 0, 0, 0, 0));
            UNIT_ASSERT(!it.Valid());

            it.SeekToFirst();
            it.Prev();
            UNIT_ASSERT(!it.Valid());

            it.SeekToLast();
            it.Next();
            UNIT_ASSERT(!it.Valid());

            it.SeekToFirst();
            TStringStream str1;
            while (it.Valid()) {
                str1 << it.GetCurKey().ToString();
                it.Next();
            }
            UNIT_ASSERT(str1.Str()
                == "[10:0:0:0:0:0:0][10:0:10:0:0:0:0][20:0:0:0:0:0:0][20:0:10:0:0:0:0][20:0:300:0:0:0:0]");

            it.SeekToLast();
            TStringStream str2;
            while (it.Valid()) {
                str2 << it.GetCurKey().ToString();
                it.Prev();
            }
            UNIT_ASSERT(str2.Str()
                == "[20:0:300:0:0:0:0][20:0:10:0:0:0:0][20:0:0:0:0:0:0][10:0:10:0:0:0:0][10:0:0:0:0:0:0]");
        }

        Y_UNIT_TEST(TestSstIndexSaveLoad) {
            TTestContexts ctxs;
            TTrackableVector<TLogoBlobSst::TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));

            auto addRecord = [&index](ui64 tabletId, ui32 step, ui32 blobSize) {
                TLogoBlobID id(tabletId, 0, step, 0, blobSize, 0);
                index.emplace_back(TKeyLogoBlob(id), TMemRecLogoBlob());
            };

            addRecord(10, 0, 1);
            addRecord(10, 10, 2);
            addRecord(20, 0, 3);
            addRecord(20, 10, 4);
            addRecord(20, 300, 5);

            TTwoLevelLogoBlobSstIndex twoLevel(ctxs.GetVCtx());
            twoLevel.Load(TTrackableVector<TLogoBlobSst::TRec>(index));

            const auto& indexHigh = twoLevel.IndexHigh;
            auto high = indexHigh.begin();

            using TLogoBlobIdHigh = TTwoLevelLogoBlobSstIndex::TLogoBlobIdHigh;

            UNIT_ASSERT(high->GetKey() == TLogoBlobIdHigh(10, 0, 0, 0));
            UNIT_ASSERT(high->GetLowRangeEndIndex() == 2);
            ++high;
            UNIT_ASSERT(high->GetKey() == TLogoBlobIdHigh(20, 0, 0, 0));
            UNIT_ASSERT(high->GetLowRangeEndIndex() == 4);
            ++high;
            UNIT_ASSERT(high->GetKey() == TLogoBlobIdHigh(20, 0, 300, 0));
            UNIT_ASSERT(high->GetLowRangeEndIndex() == 5);
            ++high;
            UNIT_ASSERT(high == indexHigh.end());

            const auto& indexLow = twoLevel.IndexLow;
            auto low = indexLow.begin();

            using TLogoBlobIdLow = TTwoLevelLogoBlobSstIndex::TLogoBlobIdLow;

            UNIT_ASSERT(low->GetKey() == TLogoBlobIdLow(0, 0, 0, 1, 0));
            ++low;
            UNIT_ASSERT(low->GetKey() == TLogoBlobIdLow(10, 0, 0, 2, 0));
            ++low;
            UNIT_ASSERT(low->GetKey() == TLogoBlobIdLow(0, 0, 0, 3, 0));
            ++low;
            UNIT_ASSERT(low->GetKey() == TLogoBlobIdLow(10, 0, 0, 4, 0));
            ++low;
            UNIT_ASSERT(low->GetKey() == TLogoBlobIdLow(300, 0, 0, 5, 0));
            ++low;
            UNIT_ASSERT(low == indexLow.end());

            TTrackableVector<TLogoBlobSst::TRec> checkIndex(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            twoLevel.Save(&checkIndex);

            for (auto i = index.begin(), c = checkIndex.begin(); i != index.end(); ++i, ++c) {
                UNIT_ASSERT(i->GetKey() == c->GetKey());
            }
        }
    } // TBlobStorageHullSstIt

    Y_UNIT_TEST_SUITE(TBlobStorageHullSstIndexForms) {

        using namespace NBlobStorageHullSstItHelpers;
        using TMemIterator = TLogoBlobSst::TMemIterator;
        using TRecs = TTrackableVector<TLogoBlobSst::TRec>;
        constexpr auto TwoLevel = ESstIndexFormPolicy::TwoLevel;
        constexpr auto Huffman = ESstIndexFormPolicy::Huffman;

        // stepPlus = 1: 256 records per high group; stepPlus = 256: every record is its own group
        TRecs MakeRecords(TTestContexts& ctxs, ui32 count, ui32 stepPlus) {
            TRecs recs(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            for (ui32 i = 0; i < count; ++i) {
                TLogoBlobID id(7, 1, 10 + i * stepPlus, 0, 100 + i, i % 5);
                TMemRecLogoBlob memRec;
                memRec.SetDiskBlob(TDiskPart(1, i * 4096, 100 + i));
                recs.emplace_back(TKeyLogoBlob(id), memRec);
            }
            return recs;
        }

        TString Walk(const TLogoBlobSst& sst, bool forward) {
            TStringStream str;
            TMemIterator it(&sst);
            forward ? it.SeekToFirst() : it.SeekToLast();
            while (it.Valid()) {
                str << it.GetCurKey().ToString() << ":" << it.GetMemRec().DataSize() << " ";
                forward ? it.Next() : it.Prev();
            }
            return str.Str();
        }

        Y_UNIT_TEST(PolicyPicksTheForm) {
            TTestContexts ctxs;
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                sst->LoadLinearIndex(MakeRecords(ctxs, 100, 1));
                UNIT_ASSERT_VALUES_EQUAL(TString(sst->IndexFormName()), kind == TwoLevel ? "TwoLevel" : "Huffman");
                UNIT_ASSERT_VALUES_EQUAL(sst->Elements(), 100);
                UNIT_ASSERT(sst->IsLoaded());
            }
            UNIT_ASSERT_VALUES_EQUAL(TString(TLogoBlobSst(ctxs.GetVCtx()).IndexFormName()), "TwoLevel");
        }

        Y_UNIT_TEST(OtherKeysAlwaysUseFlat) {
            TTestContexts ctxs;
            using TBlocksSst = TLevelSegment<TKeyBlock, TMemRecBlock>;
            static_assert(std::is_same_v<TBlocksSst::TIndex, TFlatSstIndex<TKeyBlock, TMemRecBlock>>);
            TTrackableVector<TBlocksSst::TRec> recs(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            recs.emplace_back(TKeyBlock(1), TMemRecBlock(1));
            recs.emplace_back(TKeyBlock(2), TMemRecBlock(2));
            TIntrusivePtr<TBlocksSst> sst(new TBlocksSst(ctxs.GetVCtx()));
            sst->LoadLinearIndex(std::move(recs));
            UNIT_ASSERT_VALUES_EQUAL(TString(sst->IndexFormName()), "Flat");
            UNIT_ASSERT_VALUES_EQUAL(sst->Elements(), 2);
            UNIT_ASSERT_VALUES_EQUAL(sst->FirstKey().ToString(), TKeyBlock(1).ToString());
            UNIT_ASSERT_VALUES_EQUAL(sst->LastKey().ToString(), TKeyBlock(2).ToString());
        }

        Y_UNIT_TEST(FormsIterateIdentically) {
            TTestContexts ctxs;
            for (ui32 stepPlus : {1u, 3u, 256u}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = TwoLevel;
                TLogoBlobSstPtr a(new TLogoBlobSst(ctxs.GetVCtx()));
                a->LoadLinearIndex(MakeRecords(ctxs, 300, stepPlus));
                ctxs.GetVCtx()->SstIndexFormPolicy = Huffman;
                TLogoBlobSstPtr b(new TLogoBlobSst(ctxs.GetVCtx()));
                b->LoadLinearIndex(MakeRecords(ctxs, 300, stepPlus));
                UNIT_ASSERT_VALUES_EQUAL(TString(a->IndexFormName()), "TwoLevel");
                UNIT_ASSERT_VALUES_EQUAL(TString(b->IndexFormName()), "Huffman");

                UNIT_ASSERT_VALUES_EQUAL(Walk(*a, true), Walk(*b, true));
                UNIT_ASSERT_VALUES_EQUAL(Walk(*a, false), Walk(*b, false));
                UNIT_ASSERT_VALUES_EQUAL(a->FirstKey().ToString(), b->FirstKey().ToString());
                UNIT_ASSERT_VALUES_EQUAL(a->LastKey().ToString(), b->LastKey().ToString());

                // seeks: present keys, keys between records, before the first, after the last
                for (ui32 i = 0; i < 300 * stepPlus + 2; ++i) {
                    TKeyLogoBlob key(TLogoBlobID(7, 1, 9 + i, 0, 0, 0));
                    TMemIterator ia(a.Get());
                    TMemIterator ib(b.Get());
                    ia.Seek(key);
                    ib.Seek(key);
                    UNIT_ASSERT_VALUES_EQUAL(ia.Valid(), ib.Valid());
                    if (ia.Valid()) {
                        UNIT_ASSERT_VALUES_EQUAL(ia.GetCurKey().ToString(), ib.GetCurKey().ToString());
                        UNIT_ASSERT_VALUES_EQUAL(ia.GetMemRec().DataSize(), ib.GetMemRec().DataSize());
                        UNIT_ASSERT(key <= ia.GetCurKey());
                        ia.Prev();
                        ib.Prev();
                        UNIT_ASSERT_VALUES_EQUAL(ia.Valid(), ib.Valid());
                        if (ia.Valid()) {
                            UNIT_ASSERT(ia.GetCurKey() < key);
                        }
                    }
                }
            }
        }

        Y_UNIT_TEST(IteratorEqualityAndCopy) {
            TTestContexts ctxs;
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                sst->LoadLinearIndex(MakeRecords(ctxs, 50, 256));
                TMemIterator a(sst.Get());
                a.SeekToFirst();
                a.Next();
                TMemIterator b = a;
                UNIT_ASSERT(a == b);
                b.Next();
                UNIT_ASSERT(a != b);
                a.Next();
                UNIT_ASSERT(a == b);
                TMemIterator c(sst.Get());
                c.Seek(a.GetCurKey());
                UNIT_ASSERT(a == c);
                UNIT_ASSERT(!TMemIterator().Valid());
            }
        }

        // the states other than "at a record" that callers rely on, in every form
        Y_UNIT_TEST(IteratorEdgeStates) {
            TTestContexts ctxs;
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                TRecs recs = MakeRecords(ctxs, 130, 256);
                const TString first = recs.front().GetKey().ToString();
                const TString last = recs.back().GetKey().ToString();
                TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                sst->LoadLinearIndex(std::move(recs));
                TMemIterator it(sst.Get());

                // past-the-end, then back to the last record
                it.Seek(TLogoBlobID(Max<ui64>(), Max<ui32>(), Max<ui32>(), 255, 0, 0));
                UNIT_ASSERT(!it.Valid());
                it.Prev();
                UNIT_ASSERT(it.Valid());
                UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), last);
                it.Next();
                UNIT_ASSERT(!it.Valid());
                it.Prev();
                UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), last);

                // before-begin, then sought again
                it.SeekToFirst();
                UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), first);
                it.Prev();
                UNIT_ASSERT(!it.Valid());
                it.Seek(TLogoBlobID(0, 0, 0, 0, 0, 0));
                UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), first);

                // a copy moves on its own
                TMemIterator copy = it;
                copy.Next();
                UNIT_ASSERT_VALUES_EQUAL(it.GetCurKey().ToString(), first);
                UNIT_ASSERT(copy.Valid() && copy.GetCurKey().ToString() != first);
            }
        }

        Y_UNIT_TEST(EmptyIndexInEveryForm) {
            TTestContexts ctxs;
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                sst->LoadLinearIndex(TRecs(TMemoryConsumer(ctxs.GetVCtx()->SstIndex)));
                UNIT_ASSERT(!sst->IsLoaded());
                TMemIterator it(sst.Get());
                it.SeekToFirst();
                UNIT_ASSERT(!it.Valid());
                it.Seek(TLogoBlobID(1, 1, 1, 0, 0, 0));
                UNIT_ASSERT(!it.Valid());
            }
        }

        Y_UNIT_TEST(MemoryIsReleasedInEveryForm) {
            TTestContexts ctxs;
            auto counter = ctxs.GetVCtx()->SstIndex.GetCounter();
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                TRecs recs = MakeRecords(ctxs, 1000, 3);
                const i64 linearBytes = recs.capacity() * sizeof(TLogoBlobSst::TRec);
                const i64 before = counter->Val();
                {
                    TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                    sst->LoadLinearIndex(std::move(recs));
                    // the linear input has been freed, the form is accounted
                    UNIT_ASSERT_VALUES_EQUAL(counter->Val() - before + linearBytes, i64(sst->IndexMemoryBytes()));
                }
                UNIT_ASSERT_VALUES_EQUAL(counter->Val() - before, -linearBytes);
            }
        }

        Y_UNIT_TEST(SaveLinearIndexRoundTripsBothForms) {
            TTestContexts ctxs;
            for (auto kind : {TwoLevel, Huffman}) {
                ctxs.GetVCtx()->SstIndexFormPolicy = kind;
                for (ui32 stepPlus : {1u, 256u}) {
                    TRecs recs = MakeRecords(ctxs, 200, stepPlus);
                    TLogoBlobSstPtr sst(new TLogoBlobSst(ctxs.GetVCtx()));
                    sst->LoadLinearIndex(TRecs(recs));
                    TRecs saved(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
                    sst->SaveLinearIndex(&saved);
                    UNIT_ASSERT_VALUES_EQUAL(saved.size(), recs.size());
                    for (size_t i = 0; i < recs.size(); ++i) {
                        UNIT_ASSERT(saved[i].GetKey() == recs[i].GetKey());
                        UNIT_ASSERT_VALUES_EQUAL(saved[i].GetMemRec().DataSize(), recs[i].GetMemRec().DataSize());
                    }
                }
            }
        }
    }

    Y_UNIT_TEST_SUITE(TBlobStorageHullOrderedSstsIt) {

        using namespace NBlobStorageHullSstItHelpers;
        using TIterator = TLogoBlobOrderedSsts::TReadIterator;
        TTestContexts TestCtx(ChunkSize, CompWorthReadSize);

        Y_UNIT_TEST(TestSeekToFirst) {
            TLogoBlobOrderedSstsPtr ptr(GenerateOrderedSsts(10, 5, 1, 3));
            THullCtxPtr hullCtx = TestCtx.GetHullCtx();
            TIterator it(hullCtx, ptr.Get());
            it.SeekToFirst();

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Next();
            }
            TString result("[0:0:10:0:0:0:0][0:0:11:0:0:0:0]"
                          "[0:0:12:0:0:0:0][0:0:13:0:0:0:0]"
                          "[0:0:14:0:0:0:0][0:0:15:0:0:0:0]"
                          "[0:0:16:0:0:0:0][0:0:17:0:0:0:0]"
                          "[0:0:18:0:0:0:0][0:0:19:0:0:0:0]"
                          "[0:0:20:0:0:0:0][0:0:21:0:0:0:0]"
                          "[0:0:22:0:0:0:0][0:0:23:0:0:0:0]"
                          "[0:0:24:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekToLast) {
            TLogoBlobOrderedSstsPtr ptr(GenerateOrderedSsts(10, 5, 1, 3));
            THullCtxPtr hullCtx = TestCtx.GetHullCtx();
            TIterator it(hullCtx, ptr.Get());
            it.SeekToLast();

            TStringStream str;
            while (it.Valid()) {
                str << it.GetCurKey().ToString();
                it.Prev();
            }
            TString result("[0:0:24:0:0:0:0][0:0:23:0:0:0:0]"
                          "[0:0:22:0:0:0:0][0:0:21:0:0:0:0]"
                          "[0:0:20:0:0:0:0][0:0:19:0:0:0:0]"
                          "[0:0:18:0:0:0:0][0:0:17:0:0:0:0]"
                          "[0:0:16:0:0:0:0][0:0:15:0:0:0:0]"
                          "[0:0:14:0:0:0:0][0:0:13:0:0:0:0]"
                          "[0:0:12:0:0:0:0][0:0:11:0:0:0:0]"
                          "[0:0:10:0:0:0:0]");
            UNIT_ASSERT(str.Str() == result);
        }

        Y_UNIT_TEST(TestSeekAfterAndPrev) {
            TLogoBlobOrderedSstsPtr ptr(GenerateOrderedSsts(10, 5, 1, 3));
            THullCtxPtr hullCtx = TestCtx.GetHullCtx();
            TIterator it(hullCtx, ptr.Get());

            TLogoBlobID id;
            id = TLogoBlobID(0, 0, 30, 0, 0, 0);
            it.Seek(id);
            UNIT_ASSERT(!it.Valid());
            it.Prev();
            UNIT_ASSERT(it.Valid());
            UNIT_ASSERT(it.GetCurKey().ToString() == "[0:0:24:0:0:0:0]");
        }

        // FIXME: not all cases covered
    }

    Y_UNIT_TEST_SUITE(TBlobStorageHullSstHeapStripe) {
        Y_UNIT_TEST(StripeIsDerivedFromChunkOwnership) {
            TTestContexts ctxs;
            using TSst = TLevelSegment<TKeyBlock, TMemRecBlock>;
            TIntrusivePtr<TSst> seg(new TSst(ctxs.GetVCtx()));
            seg->LastPartAddr = TDiskPart(7, 4064, 80);
            seg->HeapStripe = seg->LastPartAddr;
            seg->AllChunks = {7};

            // nothing about the stripe is written down; the SST address is the whole record
            NKikimrVDiskData::TDiskPart pb;
            seg->SerializeToProto(pb);
            UNIT_ASSERT_VALUES_EQUAL(pb.GetChunkIdx(), 7u);
            UNIT_ASSERT_VALUES_EQUAL(pb.GetOffset(), 4064u);
            UNIT_ASSERT_VALUES_EQUAL(pb.GetSize(), 80u);

            TSst loaded(ctxs.GetVCtx(), pb);
            UNIT_ASSERT(loaded.HeapStripe.Empty());

            // a chunk owned by the slot heap leaves the SST unstriped
            loaded.ResolveHeapStripe(THashSet<TChunkIdx>{9});
            UNIT_ASSERT(loaded.HeapStripe.Empty());

            loaded.ResolveHeapStripe(THashSet<TChunkIdx>{7});
            UNIT_ASSERT_VALUES_EQUAL(loaded.HeapStripe.ChunkIdx, 7u);
            UNIT_ASSERT_VALUES_EQUAL(loaded.HeapStripe.Offset, 4064u);
            UNIT_ASSERT_VALUES_EQUAL(loaded.HeapStripe.Size, 80u);

            TVector<ui32> ids;
            loaded.FillInChunkIds(ids);
            UNIT_ASSERT(ids.empty());

            loaded.AllChunks = {7};
            TSet<TChunkIdx> chunks;
            loaded.GetOwnedChunks(chunks);
            UNIT_ASSERT(chunks.contains(7));

            // a second stripe SST in the same chunk is allowed to claim it again
            TSst other(ctxs.GetVCtx());
            other.AllChunks = {7};
            other.HeapStripe = loaded.HeapStripe;
            other.GetOwnedChunks(chunks);
            UNIT_ASSERT_VALUES_EQUAL(chunks.size(), 1u);
        }
    }

} // NKikimr
