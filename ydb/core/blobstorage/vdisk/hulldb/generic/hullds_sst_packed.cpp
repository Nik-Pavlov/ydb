#include "hullds_sst_packed.h"

#include <util/generic/algorithm.h>

#include <bit>

namespace NKikimr {

    namespace {

        using E = TPackedLogoBlobSstIndex;

        // the value of column `C` in a linear record
        template <E::EColumn C>
        ui64 ValueOf(const E::TRec& rec) {
            if constexpr (C == E::Step || C == E::PartCrc || C == E::BlobSize || C == E::Cookie) {
                const TLogoBlobID id = rec.GetKey().LogoBlobID();
                if constexpr (C == E::Step) {
                    return id.Step();
                } else if constexpr (C == E::PartCrc) {
                    return id.PartId() | id.CrcMode() << 4;
                } else if constexpr (C == E::BlobSize) {
                    return id.BlobSize();
                } else {
                    return id.Cookie();
                }
            } else {
                const TMemRecLogoBlob memRec = rec.GetMemRec();
                E::TMemRecFields fields;
                memcpy(&fields, &memRec, sizeof(fields));
                if constexpr (C == E::Ingress) {
                    return fields.Ingress;
                } else if constexpr (C == E::TypeId) {
                    return fields.TypeId;
                } else if constexpr (C == E::Offset) {
                    return fields.Offset;
                } else {
                    return fields.Size;
                }
            }
        }

    } // anonymous

    TPackedLogoBlobSstIndex::TPackedLogoBlobSstIndex(const TMemoryConsumer& consumer,
            const TTrackableVector<TRec>& linearIndex)
        : Records(linearIndex.size())
        , IndexHigh(BuildLogoBlobSstIndexHigh(consumer, linearIndex))
        , Bits(TMemoryConsumer(consumer))
        , Dicts(TMemoryConsumer(consumer))
        , Frames(TMemoryConsumer(consumer))
    {
        TBitWriter writer;
        Pack<Step>(writer, linearIndex);
        Pack<PartCrc>(writer, linearIndex);
        Pack<BlobSize>(writer, linearIndex);
        Pack<Cookie>(writer, linearIndex);
        Pack<Ingress>(writer, linearIndex);
        Pack<TypeId>(writer, linearIndex);
        Pack<Offset>(writer, linearIndex);
        Pack<Size>(writer, linearIndex);

        // Assigning words to Bits to avoid calling shrink_to_fit
        const TVector<ui64> words = std::move(writer).Finish();
        Bits.assign(words.begin(), words.end());
        Dicts.shrink_to_fit();
        Frames.shrink_to_fit();
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
            dict.push_back(ValueOf<C>(rec));
        }
        SortUnique(dict);

        TColumn& col = Columns[C];
        col.Base = Dicts.size();
        col.BitBase = writer.GetBits();
        col.Width = std::bit_width(dict.size() - 1);
        Dicts.insert(Dicts.end(), dict.begin(), dict.end());
        for (const TRec& rec : linearIndex) {
            writer.Write(LowerBound(dict.begin(), dict.end(), ValueOf<C>(rec)) - dict.begin(), col.Width);
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
                const ui64 value = ValueOf<C>(linearIndex[i]);
                min = Min(min, value);
                max = Max(max, value);
            }
            const ui8 width = std::bit_width(max - min);
            Frames.push_back(TFrame{.BitBase = ui32(writer.GetBits()), .ValueBase = ui32(min), .Width = width});
            for (size_t i = begin; i < end; ++i) {
                writer.Write(ValueOf<C>(linearIndex[i]) - min, width);
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
