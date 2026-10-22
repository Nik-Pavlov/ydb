#include "hullds_sst_coded.h"
#include "hullds_sst_coded_ut.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {

    using namespace NSstIndexTest;

    Y_UNIT_TEST_SUITE(TCodedLogoBlobIndexTest) {

        Y_UNIT_TEST(RoundTrip) {
            TTestContexts ctxs;
            CheckRoundTrips<TCodedLogoBlobIndex>(ctxs);
        }

        Y_UNIT_TEST(EveryRecordItsOwnGroup) {
            TTestContexts ctxs;
            CheckEveryRecordItsOwnGroup<TCodedLogoBlobIndex>(ctxs);
        }

        Y_UNIT_TEST(StepJumpsAndRepeats) {
            TTestContexts ctxs;
            CheckStepJumpsAndRepeats<TCodedLogoBlobIndex>(ctxs);
        }

        Y_UNIT_TEST(SeekBetweenGroups) {
            TTestContexts ctxs;
            CheckSeekBetweenGroups<TCodedLogoBlobIndex>(ctxs);
        }

        Y_UNIT_TEST(OffsetWithoutData) {
            TTestContexts ctxs;
            CheckOffsetWithoutData<TCodedLogoBlobIndex>(ctxs);
        }

        Y_UNIT_TEST(GroupsWithinBlock) {
            // blocks that hold records of several groups with Steps far apart, the smallest Step of a block not in
            // its first record: every Step is relative to the smallest one of its block
            TTestContexts ctxs;
            TTrackableVector<TRec> index(TMemoryConsumer(ctxs.GetVCtx()->SstIndex));
            for (ui32 tablet : {1u, 2u, 3u}) {
                const ui32 count = tablet == 2 ? 100 : 10;
                for (ui32 i = 0; i < count; ++i) {
                    TMemRecLogoBlob memRec;
                    memRec.SetDiskBlob(TDiskPart(1, i * 4096, 100));
                    const ui32 step = (4 - tablet) * 1000000 + i * i * 37; // each group below the one before
                    index.emplace_back(TKeyLogoBlob(TLogoBlobID(tablet, 1, step, 0, 100, 0)), memRec);
                }
            }
            CheckIndex<TCodedLogoBlobIndex>(ctxs, index);
        }

    }

} // NKikimr
