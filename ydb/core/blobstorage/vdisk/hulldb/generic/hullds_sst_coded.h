#pragma once

#include "defs.h"
#include "hullds_sst_codec.h"

#include <ydb/core/blobstorage/vdisk/common/memusage.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_logoblob.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_rec.h>

namespace NKikimr {

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TCodedLogoBlobIndex -- the LogoBlob SST index, entropy coded
    //
    // Every record is coded on its own: each key and memrec field with its own TValueCode, built per SST from the
    // values the field takes, except Offset, which is stored raw with a width fixed per SST, and only when the record
    // has data. An anchor per block of BlockSize records gives the position of every record of the block in the stream
    // (see TAnchor), so any record can be decoded directly: a seek is a binary search over records, and moving to the
    // next or the previous record is O(1).
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    class TCodedLogoBlobIndex {
    public:
        using TRec = TIndexRecord<TKeyLogoBlob, TMemRecLogoBlob>;

        static constexpr ui32 BlockSize = 64;

        enum EField : ui32 {
            Tablet,
            Channel,
            Gen,
            Step,
            PartCrc,        // PartId | CrcMode << 4
            BlobSize,
            Cookie,
            Ingress,
            TypeId,         // Type:2 | Id:30 of TMemRecLogoBlob
            Size,
            FieldCount
        };

        static_assert(FieldCount == 10, "Invalid field count");

        // codes a non-empty linear index
        TCodedLogoBlobIndex(const TMemoryConsumer& consumer, const TTrackableVector<TRec>& linearIndex);

        void SaveLinearIndex(TTrackableVector<TRec> *linearIndex) const;

        ui64 GetRecords() const {
            return Records;
        }

        // memory the index takes: stream, anchors and code tables
        ui64 GetBytes() const {
            return Stream.capacity() * sizeof(ui64) + Anchors.capacity() * sizeof(TAnchor) + TableBytes;
        }

        TKeyLogoBlob FirstKey() const {
            return IndexFirstKey;
        }

        TKeyLogoBlob LastKey() const {
            return IndexLastKey;
        }

        // the position of the first record with a key >= `key`, GetRecords() if there is none
        ui64 LowerBound(const TKeyLogoBlob& key) const;

        // decodes record `index` < GetRecords()
        void Decode(ui64 index, TKeyLogoBlob *key, TMemRecLogoBlob *memRec) const;

    private:
        // Where the records of a block start in Stream: record 0 at Base, record j > 0 at Base + Pos[j - 1]. A record
        // takes under 1000 bits, so a block fits ui16 positions.
        struct TAnchor {
            ui32 Base = 0;
            std::array<ui16, BlockSize - 1> Pos{};
        };

        ui64 Records = 0;
        TKeyLogoBlob IndexFirstKey;
        TKeyLogoBlob IndexLastKey;
        TTrackableVector<ui64> Stream;
        TTrackableVector<TAnchor> Anchors;
        std::array<NSstCodec::TValueCode, FieldCount> Codes;
        ui32 OffsetWidth = 0;
        // set when some record without data still has an offset, so that every record stores it
        bool OffsetInAllRecords = false;
        TMemoryConsumerWithDropOnDestroy TableMemory;
        ui64 TableBytes = 0;

        bool HasOffset(ui64 typeId) const {
            return typeId || OffsetInAllRecords;
        }

        // a reader at the start of record `index`
        NSstCodec::TBitReader ReaderAt(ui64 index) const;

        TKeyLogoBlob DecodeKey(NSstCodec::TBitReader& reader) const;
    };

} // NKikimr
