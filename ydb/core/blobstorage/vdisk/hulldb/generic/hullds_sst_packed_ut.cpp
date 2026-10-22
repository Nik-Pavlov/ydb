#include "hullds_sst_packed.h"
#include "hullds_sst_coded_ut.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {

    using namespace NSstIndexTest;

    Y_UNIT_TEST_SUITE(TPackedLogoBlobSstIndexTest) {

        Y_UNIT_TEST(RoundTrip) {
            TTestContexts ctxs;
            CheckRoundTrips<TPackedLogoBlobSstIndex>(ctxs);
        }

        Y_UNIT_TEST(EveryRecordItsOwnGroup) {
            TTestContexts ctxs;
            CheckEveryRecordItsOwnGroup<TPackedLogoBlobSstIndex>(ctxs);
        }

        Y_UNIT_TEST(StepJumpsAndRepeats) {
            TTestContexts ctxs;
            CheckStepJumpsAndRepeats<TPackedLogoBlobSstIndex>(ctxs);
        }

        Y_UNIT_TEST(SeekBetweenGroups) {
            TTestContexts ctxs;
            CheckSeekBetweenGroups<TPackedLogoBlobSstIndex>(ctxs);
        }

        Y_UNIT_TEST(OffsetWithoutData) {
            TTestContexts ctxs;
            CheckOffsetWithoutData<TPackedLogoBlobSstIndex>(ctxs);
        }

    }

} // NKikimr
