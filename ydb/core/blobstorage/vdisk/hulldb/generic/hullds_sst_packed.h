#pragma once

#include "defs.h"
#include "hullds_sst_bitstream.h"
#include "hullds_sst_indexhigh.h"

#include <ydb/core/blobstorage/vdisk/common/memusage.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_logoblob.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullbase_rec.h>

namespace NKikimr {

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TPackedLogoBlobSstIndex - the two-level LogoBlob SST index with the low level packed into fixed-width columns.
    //
    // The high level is the two-level form's TRecHigh per (TabletId, Channel, Generation) group. The low level is a
    // fixed-width column per field, so any record decodes directly from its index. A column holds either indexes into
    // a sorted dictionary of the field's values or offsets from a base per frame of FrameSize records (see Kinds).
    //
    // The memrec's Size is not stored: the record's type and local parts give the data parts it holds (see DataParts),
    // BlobSize gives their size, and the Header column keeps the rest, the bytes of the blob headers. A record whose
    // Size is not what its parts and headers take aborts the build.
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    class TPackedLogoBlobSstIndex {
    public:
        using TRec = TIndexRecord<TKeyLogoBlob, TMemRecLogoBlob>;

        using TRecHigh = TLogoBlobSstRecHigh;

        // Records per frame of a Frame column
        static constexpr ui32 FrameSize = 32;

        enum EColumn : ui32 {
            Step,
            PartCrc,        // PartId | CrcMode << 4
            BlobSize,
            Cookie,
            Ingress,
            TypeId,         // Type:2 | Id:30 of TMemRecLogoBlob
            Offset,
            Header,         // Size - the bytes of the data parts: the blob headers, none without data
            ColumnCount
        };

        enum class EKind : ui8 {
            Dict,           // An index into the sorted distinct values of the SST
            Frame,          // value - base of the record's frame, with a width per frame; for ui32 fields only
        };

        // Chosen on production index snapshots: Step and Cookie frame well (steps of 32 neighbouring records of one
        // group span a narrow range, cookies are mostly zero), BlobSize, Ingress and TypeId take few distinct values
        // per SST, PartCrc even fewer, and Header one to three (no data and the header modes of the SST). Offset grows
        // with the records of an SST; a frame bounds its cost at raw plus 9 bytes per frame even when it does not.
        static constexpr std::array<EKind, ColumnCount> Kinds = {
            EKind::Frame,   // Step
            EKind::Dict,    // PartCrc
            EKind::Dict,    // BlobSize
            EKind::Frame,   // Cookie
            EKind::Dict,    // Ingress
            EKind::Dict,    // TypeId
            EKind::Frame,   // Offset
            EKind::Dict,    // Header
        };

        // TMemRecLogoBlob as stored: ingress, Type:2 | Id:30, Offset, Size
#pragma pack(push, 4)
        struct TMemRecFields {
            ui64 Ingress;
            ui32 TypeId;
            ui32 Offset;
            ui32 Size;
        };
#pragma pack(pop)
        static_assert(sizeof(TMemRecFields) == sizeof(TMemRecLogoBlob));

        // Aborts if a record's Size is unexpected, see IsSizeExpected
        TPackedLogoBlobSstIndex(const TMemoryConsumer& consumer, TBlobStorageGroupType gtype,
            const TTrackableVector<TRec>& linearIndex);

        // whether the Size of `rec` is what its data parts and blob headers take, so that the index can derive it
        static bool IsSizeExpected(TBlobStorageGroupType gtype, const TRec& rec);

        void Save(TTrackableVector<TRec> *linearIndex) const;

        ui64 Elements() const {
            return Records;
        }

        TKeyLogoBlob FirstKey() const {
            return KeyAt(IndexHigh.front(), 0);
        }

        TKeyLogoBlob LastKey() const {
            return KeyAt(IndexHigh.back(), Records - 1);
        }

        // the key of record `index`, which belongs to group `high`
        TKeyLogoBlob KeyAt(const TRecHigh& high, ui64 index) const {
            const ui64 step = Get<Step>(index);
            return TKeyLogoBlob(TLogoBlobID(high.TabletId, high.ChannelGeneration << 24 | step >> 8, Raw2At(index, step)));
        }

        TMemRecLogoBlob MemRecAt(ui64 index) const {
            const ui64 ingress = CodeAt<Ingress>(index);
            TMemRecFields fields;
            fields.Ingress = Dicts[Columns[Ingress].Base + ingress];
            fields.TypeId = Get<TypeId>(index);
            fields.Offset = Get<Offset>(index);
            fields.Size = 0;
            TMemRecLogoBlob memRec;
            memcpy(&memRec, &fields, sizeof(fields));
            fields.Size = DataParts(memRec.GetType(), LocalDataParts[ingress], fields.Offset) * PartSizeAt(index)
                + Get<Header>(index);
            memcpy(&memRec, &fields, sizeof(fields));
            return memRec;
        }

        // whether record `index` orders before the key with the given Step and Raw[2], both in the record's group
        bool LowKeyLess(ui64 index, ui64 step, ui64 raw2) const {
            const ui64 recStep = Get<Step>(index);
            return recStep != step ? recStep < step : Raw2At(index, recStep) < raw2;
        }

        class TIterator;

    private:
        // where a frame starts and what it counts from
        struct TFrame {
            ui32 BitBase;
            ui32 ValueBase;
            ui8 Width;
        } Y_PACKED;

        static_assert(sizeof(TFrame) == 9, "expect sizeof(TFrame) == 9");

        struct TColumn {
            ui32 Base = 0;      // Dict: the first entry in Dicts; Frame: the first frame in Frames
            ui32 BitBase = 0;   // Dict: the bits of record 0
            ui8 Width = 0;      // Dict
        };

        // the data parts of a record, PartSize bytes each, given its local parts with data
        static ui32 DataParts(TBlobType::EType type, ui8 localDataParts, ui32 offset) {
            switch (type) {
                case TBlobType::DiskBlob:
                    return localDataParts;
                case TBlobType::HugeBlob:
                    return 1;
                case TBlobType::ManyHugeBlobs:
                    // Offset is the number of huge blobs, one part each
                    return offset;
                case TBlobType::MemBlob:
                    // only fresh segments keep data in memory
                    break;
            }
            return 0;
        }

        const ui64 Records;
        const TBlobStorageGroupType GType;
        const TTrackableVector<TRecHigh> IndexHigh;
        TTrackableVector<ui64> Bits;        // All columns, one after another
        TTrackableVector<ui64> Dicts;       // The dictionaries of the Dict columns, one after another
        TTrackableVector<TFrame> Frames;    // The frames of the Frame columns, one after another
        TTrackableVector<ui8> LocalDataParts;   // By Ingress dictionary entry
        TTrackableVector<ui32> PartSizes;   // By BlobSize dictionary entry, if all records share a CrcMode
        std::array<TColumn, ColumnCount> Columns;

        // the local parts with data; not, say, part 3 of mirror-3of4
        static ui8 LocalDataPartsOf(TBlobStorageGroupType gtype, ui64 ingress);

        // the bytes of the data parts of `rec`
        static ui64 PartBytes(TBlobStorageGroupType gtype, const TRec& rec);

        static ui32 PartSize(TBlobStorageGroupType gtype, ui32 crcMode, ui32 blobSize) {
            return gtype.TErasureType::PartSize(TErasureType::ECrcMode(crcMode), blobSize);
        }

        // the value of column `C` in a linear record
        template <EColumn C>
        static ui64 ValueOf(TBlobStorageGroupType gtype, const TRec& rec);

        template <EColumn C>
        void Pack(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex);

        template <EColumn C>
        void PackDict(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex);

        template <EColumn C>
        void PackFrame(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex);

        // the dictionary entry of Dict column `C` in record `index`
        template <EColumn C>
        ui64 CodeAt(ui64 index) const {
            static_assert(Kinds[C] == EKind::Dict);
            const TColumn& col = Columns[C];
            return ReadBits(Bits.data(), col.BitBase + index * col.Width, col.Width);
        }

        // the value of `C` in record `index`
        template <EColumn C>
        ui64 Get(ui64 index) const {
            const TColumn& col = Columns[C];
            if constexpr (Kinds[C] == EKind::Dict) {
                return Dicts[col.Base + CodeAt<C>(index)];
            } else {
                const TFrame& frame = Frames[col.Base + index / FrameSize];
                return frame.ValueBase
                    + ReadBits(Bits.data(), frame.BitBase + index % FrameSize * frame.Width, frame.Width);
            }
        }

        // the size of one data part of record `index`
        ui32 PartSizeAt(ui64 index) const {
            const ui64 blobSize = CodeAt<BlobSize>(index);
            if (!PartSizes.empty()) {
                return PartSizes[blobSize];
            }
            return PartSize(GType, Get<PartCrc>(index) >> 4, Dicts[Columns[BlobSize].Base + blobSize]);
        }

        // TLogoBlobID::GetRaw()[2] of record `index`, given its Step
        ui64 Raw2At(ui64 index, ui64 step) const {
            const ui64 partCrc = Get<PartCrc>(index);
            return (step & 0xFF) << 56 | Get<Cookie>(index) << 32 | (partCrc >> 4) << 30 | Get<BlobSize>(index) << 4
                | (partCrc & 0xF);
        }
    };

    ////////////////////////////////////////////////////////////////////////////
    // TPackedLogoBlobSstIndex::TIterator
    //
    // The two-level iterator with a record index in place of the low pointer; the record decodes on access.
    // Past-the-end is Pos == records at IndexHigh.end(); before-begin is Pos == Max<ui64>() (Prev() from record 0).
    ////////////////////////////////////////////////////////////////////////////
    class TPackedLogoBlobSstIndex::TIterator {
        const TPackedLogoBlobSstIndex* Index = nullptr;

        const TRecHigh* High = nullptr;
        ui64 Pos = 0;
        ui64 LowRangeBegin = 0;

    public:
        TIterator() = default;

        TIterator(const TPackedLogoBlobSstIndex* index)
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
        }

        void Prev() {
            Y_DEBUG_ABORT_UNLESS(Index && High && Pos <= Index->Records);
            if (Pos == LowRangeBegin) {
                --High;
                LowRangeBegin = High <= Index->IndexHigh.begin() ? 0 : (High - 1)->LowRangeEndIndex;
            }
            --Pos;
        }

        TKeyLogoBlob GetCurKey() const {
            Y_DEBUG_ABORT_UNLESS(Valid());
            return Index->KeyAt(*High, Pos);
        }

        TMemRecLogoBlob GetMemRec() const {
            Y_DEBUG_ABORT_UNLESS(Valid());
            return Index->MemRecAt(Pos);
        }

        void SeekToFirst() {
            High = Index->IndexHigh.begin();
            Pos = LowRangeBegin = 0;
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
        }

        bool operator == (const TIterator& it) const {
            return High == it.High && Pos == it.Pos;
        }
    };

} // NKikimr
