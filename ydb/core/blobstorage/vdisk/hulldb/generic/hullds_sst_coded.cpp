#include "hullds_sst_coded.h"

namespace NKikimr {

    using namespace NSstCodec;

    namespace {

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

        TMemRecFields Fields(const TMemRecLogoBlob& memRec) {
            TMemRecFields fields;
            memcpy(&fields, &memRec, sizeof(fields));
            return fields;
        }

        using E = TCodedLogoBlobIndex;

        // the coded fields of a record, in the order they are coded; Offset follows them, see HasOffset
        template <class TSink>
        void EmitRecord(TSink&& sink, const TCodedLogoBlobIndex::TRec& rec) {
            const TLogoBlobID blobId = rec.GetKey().LogoBlobID();
            const ui64 *raw = blobId.GetRaw();
            sink(E::Tablet, raw[0]);
            sink(E::Channel, raw[1] >> 56);
            sink(E::Gen, raw[1] >> 24 & 0xFFFFFFFF);
            sink(E::Step, (raw[1] & 0xFFFFFF) << 8 | raw[2] >> 56);
            sink(E::PartCrc, (raw[2] & 0xF) | (raw[2] >> 30 & 3) << 4);
            sink(E::BlobSize, raw[2] >> 4 & 0x3FFFFFF);
            sink(E::Cookie, raw[2] >> 32 & 0xFFFFFF);

            const TMemRecFields fields = Fields(rec.GetMemRec());
            sink(E::Ingress, fields.Ingress);
            sink(E::TypeId, fields.TypeId);
            sink(E::Size, fields.Size);
        }

    } // anonymous

    TCodedLogoBlobIndex::TCodedLogoBlobIndex(const TMemoryConsumer& consumer, const TTrackableVector<TRec>& linearIndex)
        : Records(linearIndex.size())
        , Stream(TMemoryConsumer(consumer))
        , Anchors(TMemoryConsumer(consumer))
        , TableMemory(consumer)
    {
        Y_ABORT_UNLESS(Records);
        IndexFirstKey = linearIndex.front().GetKey();
        IndexLastKey = linearIndex.back().GetKey();

        // the code of every field comes from the values the field takes in this SST
        std::array<THashMap<ui64, ui64>, FieldCount> freqs;
        ui32 maxOffset = 0;
        for (const TRec& rec : linearIndex) {
            EmitRecord([&](EField field, ui64 value) { ++freqs[field][value]; }, rec);
            const TMemRecFields fields = Fields(rec.GetMemRec());
            maxOffset = Max(maxOffset, fields.Offset);
            OffsetInAllRecords |= !fields.TypeId && fields.Offset;
        }
        TVector<TValueEncoder> encoders(Reserve(FieldCount));
        for (ui32 field = 0; field < FieldCount; ++field) {
            encoders.emplace_back(freqs[field]);
            Codes[field] = encoders.back().GetCode();
            TableBytes += Codes[field].GetBytes();
        }
        TableMemory.Add(TableBytes);
        OffsetWidth = BitWidth(maxOffset);

        TBitWriter writer;
        Anchors.reserve((Records + BlockSize - 1) / BlockSize);
        for (ui64 i = 0; i < Records; ++i) {
            Y_ABORT_UNLESS(writer.GetBits() <= Max<ui32>(), "coded SST index too large");
            if (const ui64 j = i % BlockSize) {
                const ui64 pos = writer.GetBits() - Anchors.back().Base;
                Y_ABORT_UNLESS(pos <= Max<ui16>(), "coded SST index block too large");
                Anchors.back().Pos[j - 1] = pos;
            } else {
                Anchors.push_back(TAnchor{.Base = ui32(writer.GetBits())});
            }
            EmitRecord([&](EField field, ui64 value) { encoders[field].Write(writer, value); }, linearIndex[i]);
            if (const TMemRecFields fields = Fields(linearIndex[i].GetMemRec()); HasOffset(fields.TypeId)) {
                writer.Write(fields.Offset, OffsetWidth);
            }
        }
        const TVector<ui64> words = std::move(writer).Finish();
        Stream.assign(words.begin(), words.end());
    }

    void TCodedLogoBlobIndex::SaveLinearIndex(TTrackableVector<TRec> *linearIndex) const {
        linearIndex->clear();
        linearIndex->reserve(Records);
        TKeyLogoBlob key;
        TMemRecLogoBlob memRec;
        for (ui64 i = 0; i < Records; ++i) {
            Decode(i, &key, &memRec);
            linearIndex->emplace_back(key, memRec);
        }
    }

    TBitReader TCodedLogoBlobIndex::ReaderAt(ui64 index) const {
        const TAnchor& anchor = Anchors[index / BlockSize];
        const ui64 j = index % BlockSize;
        return TBitReader(Stream.data(), anchor.Base + (j ? anchor.Pos[j - 1] : 0));
    }

    TKeyLogoBlob TCodedLogoBlobIndex::DecodeKey(TBitReader& reader) const {
        const ui64 tablet = Codes[Tablet].Read(reader);
        const ui64 channel = Codes[Channel].Read(reader);
        const ui64 gen = Codes[Gen].Read(reader);
        const ui64 step = Codes[Step].Read(reader);
        const ui64 partCrc = Codes[PartCrc].Read(reader);
        const ui64 blobSize = Codes[BlobSize].Read(reader);
        const ui64 cookie = Codes[Cookie].Read(reader);
        const ui64 raw1 = channel << 56 | gen << 24 | step >> 8;
        const ui64 raw2 = (step & 0xFF) << 56 | cookie << 32 | (partCrc >> 4) << 30 | blobSize << 4 | (partCrc & 0xF);
        return TKeyLogoBlob(TLogoBlobID(tablet, raw1, raw2));
    }

    void TCodedLogoBlobIndex::Decode(ui64 index, TKeyLogoBlob *key, TMemRecLogoBlob *memRec) const {
        Y_DEBUG_ABORT_UNLESS(index < Records);
        TBitReader reader = ReaderAt(index);
        *key = DecodeKey(reader);
        TMemRecFields fields;
        fields.Ingress = Codes[Ingress].Read(reader);
        fields.TypeId = Codes[TypeId].Read(reader);
        fields.Size = Codes[Size].Read(reader);
        fields.Offset = HasOffset(fields.TypeId) ? reader.Read(OffsetWidth) : 0;
        memcpy(memRec, &fields, sizeof(fields));
    }

    ui64 TCodedLogoBlobIndex::LowerBound(const TKeyLogoBlob& key) const {
        // decodes only the keys on the way
        ui64 begin = 0;
        ui64 end = Records;
        while (begin < end) {
            const ui64 middle = begin + (end - begin) / 2;
            TBitReader reader = ReaderAt(middle);
            if (DecodeKey(reader) < key) {
                begin = middle + 1;
            } else {
                end = middle;
            }
        }
        return begin;
    }

} // NKikimr
