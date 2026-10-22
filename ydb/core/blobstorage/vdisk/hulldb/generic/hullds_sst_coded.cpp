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

    } // anonymous

    TCodedLogoBlobIndex::TCodedLogoBlobIndex(const TMemoryConsumer& consumer, const TTrackableVector<TRec>& linearIndex)
        : Records(linearIndex.size())
        , IndexHigh(BuildLogoBlobSstIndexHigh(consumer, linearIndex))
        , Stream(TMemoryConsumer(consumer))
        , Anchors(TMemoryConsumer(consumer))
        , Codes(std::make_unique<std::array<TValueCode, FieldCount>>())
        , TableMemory(consumer)
    {
        Y_ABORT_UNLESS(Records);
        IndexFirstKey = linearIndex.front().GetKey();
        IndexLastKey = linearIndex.back().GetKey();

        Anchors.resize((Records + BlockSize - 1) / BlockSize);
        std::array<THashMap<ui64, ui64>, FieldCount> freqs;
        for (ui64 begin = 0; begin < Records; begin += BlockSize) {
            const ui64 end = Min<ui64>(Records, begin + BlockSize);
            TAnchor& anchor = Anchors[begin / BlockSize];
            for (ui64 i = begin; i < end; ++i) {
                anchor.Step = Min(anchor.Step, linearIndex[i].GetKey().LogoBlobID().Step());
            }

            for (ui64 i = begin; i < end; ++i) {
                const TLogoBlobID id = linearIndex[i].GetKey().LogoBlobID();
                ++freqs[StepDelta][id.Step() - anchor.Step];
                ++freqs[PartCrc][id.PartId() | id.CrcMode() << 4];
                ++freqs[BlobSize][id.BlobSize()];
                ++freqs[Cookie][id.Cookie()];

                const TMemRecFields fields = Fields(linearIndex[i].GetMemRec());
                ++freqs[Ingress][fields.Ingress];
                ++freqs[TypeId][fields.TypeId];
                ++freqs[Size][fields.Size];
            }
        }
        TVector<TValueEncoder> encoders(Reserve(FieldCount));
        for (ui32 field = 0; field < FieldCount; ++field) {
            encoders.emplace_back(freqs[field]);
            (*Codes)[field] = encoders.back().GetCode();
            TableBytes += (*Codes)[field].GetBytes();
        }
        TableMemory.Add(TableBytes);

        TBitWriter writer;
        for (ui64 begin = 0; begin < Records; begin += BlockSize) {
            const ui64 end = Min<ui64>(Records, begin + BlockSize);
            TAnchor& anchor = Anchors[begin / BlockSize];
            Y_ABORT_UNLESS(writer.GetBits() <= Max<ui32>(), "coded SST index too large");
            anchor.Base = static_cast<ui32>(writer.GetBits());

            for (ui64 i = begin; i < end; ++i) {
                const ui64 pos = writer.GetBits() - anchor.Base;
                Y_ABORT_UNLESS(pos <= Max<ui16>(), "coded SST index block too large");
                anchor.Pos[i - begin] = static_cast<ui16>(pos);

                const TLogoBlobID id = linearIndex[i].GetKey().LogoBlobID();
                encoders[StepDelta].Write(writer, id.Step() - anchor.Step);
                encoders[PartCrc].Write(writer, id.PartId() | id.CrcMode() << 4);
                encoders[BlobSize].Write(writer, id.BlobSize());
                encoders[Cookie].Write(writer, id.Cookie());

                const TMemRecFields fields = Fields(linearIndex[i].GetMemRec());
                encoders[Ingress].Write(writer, fields.Ingress);
                encoders[TypeId].Write(writer, fields.TypeId);
                encoders[Size].Write(writer, fields.Size);
                writer.Write(fields.Offset, 32);
            }
        }
        const TVector<ui64> words = std::move(writer).Finish();
        Stream.assign(words.begin(), words.end());
    }

    void TCodedLogoBlobIndex::Save(TTrackableVector<TRec> *linearIndex) const {
        linearIndex->clear();
        linearIndex->reserve(Records);
        TKeyLogoBlob key;
        TMemRecLogoBlob memRec;
        ui64 i = 0;
        for (const TRecHigh& high : IndexHigh) {
            for (; i < high.LowRangeEndIndex; ++i) {
                Decode(high, i, &key, &memRec);
                linearIndex->emplace_back(key, memRec);
            }
        }
    }

    TBitReader TCodedLogoBlobIndex::ReaderAt(ui64 index) const {
        const TAnchor& anchor = Anchors[index / BlockSize];
        return TBitReader(Stream.data(), anchor.Base + anchor.Pos[index % BlockSize]);
    }

    ui32 TCodedLogoBlobIndex::ReadStep(TBitReader& reader, ui64 index) const {
        return Anchors[index / BlockSize].Step + (*Codes)[StepDelta].Read(reader);
    }

    ui64 TCodedLogoBlobIndex::ReadRaw2(TBitReader& reader, ui32 step) const {
        const auto& codes = *Codes;
        const ui64 partCrc = codes[PartCrc].Read(reader);
        const ui64 blobSize = codes[BlobSize].Read(reader);
        const ui64 cookie = codes[Cookie].Read(reader);
        return ui64(step & 0xFF) << 56 | cookie << 32 | (partCrc >> 4) << 30 | blobSize << 4 | (partCrc & 0xF);
    }

    void TCodedLogoBlobIndex::Decode(const TRecHigh& high, ui64 index, TKeyLogoBlob *key,
            TMemRecLogoBlob *memRec) const {
        Y_DEBUG_ABORT_UNLESS(index < Records);
        TBitReader reader = ReaderAt(index);
        const ui32 step = ReadStep(reader, index);
        const ui64 raw2 = ReadRaw2(reader, step);
        *key = TKeyLogoBlob(TLogoBlobID(high.TabletId, high.ChannelGeneration << 24 | step >> 8, raw2));

        const auto& codes = *Codes;
        TMemRecFields fields;
        fields.Ingress = codes[Ingress].Read(reader);
        fields.TypeId = codes[TypeId].Read(reader);
        fields.Size = codes[Size].Read(reader);
        fields.Offset = reader.Read(32);
        memcpy(memRec, &fields, sizeof(fields));
    }

    bool TCodedLogoBlobIndex::LowKeyLess(ui64 index, ui32 step, ui64 raw2) const {
        TBitReader reader = ReaderAt(index);
        const ui32 recStep = ReadStep(reader, index);
        // the rest of the key only matters when the steps are equal
        return recStep != step ? recStep < step : ReadRaw2(reader, recStep) < raw2;
    }

} // NKikimr
