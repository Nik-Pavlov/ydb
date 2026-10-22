#pragma once

#include "defs.h"
#include "hullds_sst_codec.h"
#include "hullds_sst_indexhigh.h"

#include <ydb/core/blobstorage/vdisk/common/memusage.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_logoblob.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_rec.h>

#include <memory>

namespace NKikimr {

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TCodedLogoBlobIndex -- the two-level LogoBlob SST index with the low level entropy coded
    //
    // The high level is a TLogoBlobSstRecHigh per (TabletId, Channel, Generation) group, as in the two-level form.
    // Every record of the low level is coded on its own: each field with its own TValueCode, built per SST from the
    // values the field takes, except Offset, which is stored raw in 32 bits. Records are cut into blocks of
    // BlockSize; Step is coded relative to the smallest Step of the block.
    // An anchor per block keeps that Step and the position of every record of the block in the stream (see TAnchor),
    // so any record decodes directly: a seek is a search over the high level and then a binary search over the
    // records of one group, and moving to the next or the previous record is O(1).
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    class TCodedLogoBlobIndex {
    public:
        using TRec = TIndexRecord<TKeyLogoBlob, TMemRecLogoBlob>;
        using TRecHigh = TLogoBlobSstRecHigh;

        static constexpr ui32 BlockSize = 64;

        enum EField : ui32 {
            StepDelta,      // Step minus the Step of the record's anchor
            PartCrc,        // PartId | CrcMode << 4
            BlobSize,
            Cookie,
            Ingress,
            TypeId,         // Type:2 | Id:30 of TMemRecLogoBlob
            Size,
            FieldCount
        };

        static_assert(FieldCount == 7, "Invalid field count");

        // codes a non-empty linear index
        TCodedLogoBlobIndex(const TMemoryConsumer& consumer, const TTrackableVector<TRec>& linearIndex);

        void Save(TTrackableVector<TRec> *linearIndex) const;

        ui64 Elements() const {
            return Records;
        }

        TKeyLogoBlob FirstKey() const {
            return IndexFirstKey;
        }

        TKeyLogoBlob LastKey() const {
            return IndexLastKey;
        }

        // decodes record `index` of group `high`
        void Decode(const TRecHigh& high, ui64 index, TKeyLogoBlob *key, TMemRecLogoBlob *memRec) const;

        // whether record `index` orders before the key with the given Step and Raw[2] in the record's group
        bool LowKeyLess(ui64 index, ui32 step, ui64 raw2) const;

        class TIterator;

    private:
        // Where the records of a block start in Stream: record j at Base + Pos[j].
        // A record takes under 1000 bits, so a block fits ui16 positions.
        // Step is the smallest Step of the block's records.
        struct TAnchor {
            ui32 Base = 0;
            ui32 Step = Max<ui32>();
            std::array<ui16, BlockSize> Pos{};
        };

        ui64 Records = 0;
        TKeyLogoBlob IndexFirstKey;
        TKeyLogoBlob IndexLastKey;
        const TTrackableVector<TRecHigh> IndexHigh;
        TTrackableVector<ui64> Stream;
        TTrackableVector<TAnchor> Anchors;
        // on the heap: the code tables take about 4 KB, and the index is held inline in TLogoBlobSstIndex's variant
        std::unique_ptr<std::array<NSstCodec::TValueCode, FieldCount>> Codes;
        TMemoryConsumerWithDropOnDestroy TableMemory;
        ui64 TableBytes = 0;

        // a reader at the start of record `index`
        TBitReader ReaderAt(ui64 index) const;

        // reads the Step of record `index` from a reader at the start of the record
        ui32 ReadStep(TBitReader& reader, ui64 index) const;

        // reads the fields of Raw[2] that follow StepDelta and returns Raw[2]
        ui64 ReadRaw2(TBitReader& reader, ui32 step) const;
    };

    ////////////////////////////////////////////////////////////////////////////
    // TCodedLogoBlobIndex::TIterator
    //
    // The two-level iterator with a record index in place of the low pointer; the record is decoded on every move.
    // Past-the-end is Pos == records at IndexHigh.end(); before-begin is Pos == Max<ui64>() (Prev() from record 0).
    ////////////////////////////////////////////////////////////////////////////
    class TCodedLogoBlobIndex::TIterator {
        const TCodedLogoBlobIndex* Index = nullptr;

        const TRecHigh* High = nullptr;
        ui64 Pos = 0;
        ui64 LowRangeBegin = 0;

        TKeyLogoBlob Key;
        TMemRecLogoBlob MemRec;

        void Load() {
            if (Pos < Index->Records) {
                Index->Decode(*High, Pos, &Key, &MemRec);
            }
        }

    public:
        TIterator() = default;

        TIterator(const TCodedLogoBlobIndex* index)
            : Index(index)
        {}

        bool Valid() const {
            return Index && High && Pos < Index->Records;
        }

        void Next() {
            Y_DEBUG_ABORT_UNLESS(Valid());
            ++Pos;
            if (Pos == High->LowRangeEndIndex) {
                ++High;
                LowRangeBegin = Pos;
            }
            Load();
        }

        void Prev() {
            Y_DEBUG_ABORT_UNLESS(Index && High && Pos <= Index->Records);
            if (Pos == LowRangeBegin) {
                --High;
                LowRangeBegin = High <= Index->IndexHigh.begin() ? 0 : (High - 1)->LowRangeEndIndex;
            }
            --Pos;
            Load();
        }

        TKeyLogoBlob GetCurKey() const {
            Y_DEBUG_ABORT_UNLESS(Valid());
            return Key;
        }

        TMemRecLogoBlob GetMemRec() const {
            Y_DEBUG_ABORT_UNLESS(Valid());
            return MemRec;
        }

        void SeekToFirst() {
            High = Index->IndexHigh.begin();
            Pos = LowRangeBegin = 0;
            Load();
        }

        void SeekToLast() {
            High = Index->IndexHigh.end();
            Pos = LowRangeBegin = Index->Records;
            Prev();
        }

        void Seek(const TKeyLogoBlob& key) {
            const TLogoBlobID id = key.LogoBlobID();
            const TRecHigh keyHigh(id);

            High = std::lower_bound(Index->IndexHigh.begin(), Index->IndexHigh.end(), keyHigh);

            if (High == Index->IndexHigh.end()) {
                Pos = LowRangeBegin = Index->Records;
                return;
            }

            const ui64 rangeBegin = High == Index->IndexHigh.begin() ? 0 : (High - 1)->LowRangeEndIndex;

            if (!High->SameKey(keyHigh)) {
                Pos = LowRangeBegin = rangeBegin;
                Load();
                return;
            }

            // the first record of the group with a low key >= the key's
            ui64 begin = rangeBegin;
            ui64 end = High->LowRangeEndIndex;
            while (begin < end) {
                const ui64 middle = begin + (end - begin) / 2;
                if (Index->LowKeyLess(middle, id.Step(), id.GetRaw()[2])) {
                    begin = middle + 1;
                } else {
                    end = middle;
                }
            }
            Pos = begin;

            if (Pos == High->LowRangeEndIndex) {
                LowRangeBegin = Pos;
                ++High;
            } else {
                LowRangeBegin = rangeBegin;
            }
            Load();
        }

        bool operator == (const TIterator& it) const {
            return High == it.High && Pos == it.Pos;
        }
    };

} // NKikimr
