#include "hullds_sst_packed.h"

#include <ydb/core/blobstorage/vdisk/hulldb/base/blobstorage_blob.h>

#include <util/generic/algorithm.h>

#include <bit>

namespace NKikimr {

    TPackedLogoBlobSstIndex::TPackedLogoBlobSstIndex(const TMemoryConsumer& consumer, TBlobStorageGroupType gtype,
            const TTrackableVector<TRec>& linearIndex)
        : Records(linearIndex.size())
        , GType(gtype)
        , IndexHigh(BuildLogoBlobSstIndexHigh(consumer, linearIndex))
        , Bits(TMemoryConsumer(consumer))
        , Dicts(TMemoryConsumer(consumer))
        , Frames(TMemoryConsumer(consumer))
        , LocalDataParts(TMemoryConsumer(consumer))
        , PartSizes(TMemoryConsumer(consumer))
    {
        for (const TRec& rec : linearIndex) {
            const TMemRecLogoBlob memRec = rec.GetMemRec();
            Y_ABORT_UNLESS(IsSizeExpected(GType, rec), "Unexpected Size in a LogoBlob SST index record: Id# %s Type# %s"
                " Size# %" PRIu32 " LocalParts# %s", rec.GetKey().ToString().data(),
                TBlobType::TypeToStr(memRec.GetType()), memRec.DataSize(),
                memRec.GetLocalParts(GType).ToString().data());
        }

        TBitWriter writer;
        Pack<Step>(writer, linearIndex);
        Pack<PartCrc>(writer, linearIndex);
        Pack<BlobSize>(writer, linearIndex);
        Pack<Cookie>(writer, linearIndex);
        Pack<Ingress>(writer, linearIndex);
        Pack<TypeId>(writer, linearIndex);
        Pack<Offset>(writer, linearIndex);
        Pack<Header>(writer, linearIndex);

        // Assigning words to Bits to avoid calling shrink_to_fit
        const TVector<ui64> words = std::move(writer).Finish();
        Bits.assign(words.begin(), words.end());
        Dicts.shrink_to_fit();
        Frames.shrink_to_fit();
    }

    bool TPackedLogoBlobSstIndex::IsSizeExpected(TBlobStorageGroupType gtype, const TRec& rec) {
        const TMemRecLogoBlob memRec = rec.GetMemRec();
        TMemRecFields fields;
        memcpy(&fields, &memRec, sizeof(fields));

        // the blob headers on top of the data parts: one for the local parts inline, if any, and one per huge blob
        ui32 headers = 0;
        switch (memRec.GetType()) {
            case TBlobType::DiskBlob:
                headers = !memRec.GetLocalParts(gtype).Empty();
                break;
            case TBlobType::HugeBlob:
                headers = 1;
                break;
            case TBlobType::ManyHugeBlobs:
                headers = fields.Offset;
                break;
            case TBlobType::MemBlob:
                return false;
        }

        const ui64 partBytes = PartBytes(gtype, rec);
        if (fields.Size < partBytes) {
            return false;
        }

        // each header of any EBlobHeaderMode
        const ui64 headerBytes = fields.Size - partBytes;
        const ui32 oldHeader = TDiskBlob::GetBlobHeaderSize(EBlobHeaderMode::OLD_HEADER);
        const ui32 xxh3Header = TDiskBlob::GetBlobHeaderSize(EBlobHeaderMode::XXH3_64BIT_HEADER);
        static_assert(TDiskBlob::MaxHeaderSize == sizeof(ui64)); // no other header modes than these and NO_HEADER
        for (ui32 old = 0; old <= headers; ++old) {
            for (ui32 xxh3 = 0; old + xxh3 <= headers; ++xxh3) {
                if (old * oldHeader + xxh3 * xxh3Header == headerBytes) {
                    return true;
                }
            }
        }
        return false;
    }

    ui8 TPackedLogoBlobSstIndex::LocalDataPartsOf(TBlobStorageGroupType gtype, ui64 ingress) {
        const NMatrix::TVectorType parts = TIngress(ingress).LocalParts(gtype);
        // a part with no bytes for a non-empty blob holds no data
        const TLogoBlobID probe(1, 1, 1, 0, 1, 0);
        ui8 res = 0;
        for (ui8 i = parts.FirstPosition(); i != parts.GetSize(); i = parts.NextPosition(i)) {
            res += gtype.PartSize(TLogoBlobID(probe, i + 1)) != 0;
        }
        return res;
    }

    ui64 TPackedLogoBlobSstIndex::PartBytes(TBlobStorageGroupType gtype, const TRec& rec) {
        const TLogoBlobID id = rec.GetKey().LogoBlobID();
        const TMemRecLogoBlob memRec = rec.GetMemRec();
        TMemRecFields fields;
        memcpy(&fields, &memRec, sizeof(fields));
        const ui32 parts = DataParts(memRec.GetType(), LocalDataPartsOf(gtype, fields.Ingress), fields.Offset);
        return ui64(parts) * PartSize(gtype, id.CrcMode(), id.BlobSize());
    }

    template <TPackedLogoBlobSstIndex::EColumn C>
    ui64 TPackedLogoBlobSstIndex::ValueOf(TBlobStorageGroupType gtype, const TRec& rec) {
        const TLogoBlobID id = rec.GetKey().LogoBlobID();
        if constexpr (C == Step) {
            return id.Step();
        } else if constexpr (C == PartCrc) {
            return id.PartId() | id.CrcMode() << 4;
        } else if constexpr (C == BlobSize) {
            return id.BlobSize();
        } else if constexpr (C == Cookie) {
            return id.Cookie();
        } else {
            const TMemRecLogoBlob memRec = rec.GetMemRec();
            TMemRecFields fields;
            memcpy(&fields, &memRec, sizeof(fields));
            if constexpr (C == Ingress) {
                return fields.Ingress;
            } else if constexpr (C == TypeId) {
                return fields.TypeId;
            } else if constexpr (C == Offset) {
                return fields.Offset;
            } else {
                static_assert(C == Header);
                return fields.Size - PartBytes(gtype, rec);
            }
        }
    }

    template <TPackedLogoBlobSstIndex::EColumn C>
    void TPackedLogoBlobSstIndex::Pack(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex) {
        if constexpr (Kinds[C] == EKind::Dict) {
            PackDict<C>(writer, linearIndex);
        } else {
            PackFrame<C>(writer, linearIndex);
        }
        Y_ABORT_UNLESS(writer.GetBits() <= Max<ui32>(), "Packed SST index too large (4GB max)");
    }

    template <TPackedLogoBlobSstIndex::EColumn C>
    void TPackedLogoBlobSstIndex::PackDict(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex) {
        TVector<ui64> dict(Reserve(Records));
        for (const TRec& rec : linearIndex) {
            dict.push_back(ValueOf<C>(GType, rec));
        }
        SortUnique(dict);

        TColumn& col = Columns[C];
        col.Base = Dicts.size();
        col.BitBase = writer.GetBits();
        col.Width = std::bit_width(dict.size() - 1);
        Dicts.insert(Dicts.end(), dict.begin(), dict.end());
        for (const TRec& rec : linearIndex) {
            writer.Write(LowerBound(dict.begin(), dict.end(), ValueOf<C>(GType, rec)) - dict.begin(), col.Width);
        }

        if constexpr (C == Ingress) {
            LocalDataParts.reserve(dict.size());
            for (ui64 ingress : dict) {
                LocalDataParts.push_back(LocalDataPartsOf(GType, ingress));
            }
        } else if constexpr (C == BlobSize) {
            // with a CrcMode per record, as never seen in practice, PartSizeAt computes the size instead
            const ui32 crcMode = linearIndex.front().GetKey().LogoBlobID().CrcMode();
            if (AllOf(linearIndex, [&](const TRec& rec) { return rec.GetKey().LogoBlobID().CrcMode() == crcMode; })) {
                PartSizes.reserve(dict.size());
                for (ui64 blobSize : dict) {
                    PartSizes.push_back(PartSize(GType, crcMode, blobSize));
                }
            }
        }
    }

    template <TPackedLogoBlobSstIndex::EColumn C>
    void TPackedLogoBlobSstIndex::PackFrame(TBitWriter& writer, const TTrackableVector<TRec>& linearIndex) {
        // The frame's base is ui32, but Ingress is the only encoded 64-bit column
        static_assert(C != Ingress, "Frame column's values must fit ui32");
        Columns[C].Base = Frames.size();
        for (size_t begin = 0; begin < Records; begin += FrameSize) {
            const size_t end = Min<size_t>(Records, begin + FrameSize);
            ui64 min = Max<ui64>(), max = 0;
            for (size_t i = begin; i < end; ++i) {
                const ui64 value = ValueOf<C>(GType, linearIndex[i]);
                min = Min(min, value);
                max = Max(max, value);
            }
            const ui8 width = std::bit_width(max - min);
            Frames.push_back(TFrame{.BitBase = ui32(writer.GetBits()), .ValueBase = ui32(min), .Width = width});
            for (size_t i = begin; i < end; ++i) {
                writer.Write(ValueOf<C>(GType, linearIndex[i]) - min, width);
            }
        }
    }

    void TPackedLogoBlobSstIndex::Save(TTrackableVector<TRec> *linearIndex) const {
        linearIndex->clear();
        linearIndex->reserve(Records);
        ui64 i = 0;
        for (const TRecHigh& high : IndexHigh) {
            for (; i < high.LowRangeEndIndex; ++i) {
                linearIndex->emplace_back(KeyAt(high, i), MemRecAt(i));
            }
        }
    }

} // NKikimr
