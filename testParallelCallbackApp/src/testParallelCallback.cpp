/*
 * testParallelCallback.cpp
 *
 * See testParallelCallback.h for a description of what this driver measures.
 *
 * Author: Ralph Lange
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <callback.h>
#include <dbAccess.h>
#include <epicsAtomic.h>
#include <dbStaticLib.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <iocsh.h>

#include "testParallelCallback.h"
#include <epicsExport.h>

static const char *driverName = "testParallelCallback";

testParallelCallback::testParallelCallback(const char *portName, int numChannels)
   : asynPortDriver(portName,
                    1, /* maxAddr */
                    asynInt32Mask | asynDrvUserMask, /* Interface mask */
                    asynInt32Mask,                   /* Interrupt mask */
                    0, /* asynFlags: does not block, not multi-device */
                    1, /* Autoconnect */
                    0, /* Default priority */
                    0) /* Default stack size */
{
    int chan;
    char paramName[16];

    numChannels_ = (numChannels > 0) ? numChannels : 1;
    channelParam_ = (int *)calloc(numChannels_, sizeof(int));
    recordValue_ = NULL;
    prefix_ = NULL;
    callbacks_ = NULL;

    for (chan = 0; chan < numChannels_; chan++) {
        epicsSnprintf(paramName, sizeof(paramName), "CHANNEL%d", chan);
        createParam(paramName, asynParamInt32, &channelParam_[chan]);
        setIntegerParam(channelParam_[chan], 0);
    }
}

testParallelCallback::~testParallelCallback()
{
    free(channelParam_);
    free(recordValue_);
    free(prefix_);
    free(callbacks_);
}

/** Loads dbFile once per channel. The record name is built by the db file as
  * "$(P)$(N)", so the records are named <prefix>0 .. <prefix>numChannels-1 and
  * findRecords() can locate them again without any further configuration. */
int testParallelCallback::loadRecords(const char *dbFile, const char *macros,
                                      const char *prefix)
{
    static const char *functionName = "loadRecords";
    int chan;
    char subs[256];

    free(prefix_);
    prefix_ = epicsStrDup(prefix);

    for (chan = 0; chan < numChannels_; chan++) {
        epicsSnprintf(subs, sizeof(subs), "%s%sP=%s,N=%d",
                      macros ? macros : "",
                      (macros && macros[0]) ? "," : "",
                      prefix, chan);
        if (dbLoadRecords(dbFile, subs) != 0) {
            printf("%s:%s: dbLoadRecords(\"%s\", \"%s\") failed\n",
                   driverName, functionName, dbFile, subs);
            return -1;
        }
    }
    printf("%s:%s: port=%s loaded %d records %s0 .. %s%d\n",
           driverName, functionName, portName, numChannels_,
           prefix, prefix, numChannels_ - 1);
    return numChannels_;
}

/** Looks up the VAL field of every channel's record once and keeps a pointer
  * to it. bench() polls those pointers to find out when a round has been
  * worked off. The poll deliberately does not take the record lock: a
  * dbScanLock() per poll and record would compete with the very callback
  * threads whose scaling is being measured, whereas reading the epicsInt32
  * VAL field is a single aligned load that can at worst be one round stale,
  * which only ever makes the measured round time longer, never shorter. */
asynStatus testParallelCallback::findRecords()
{
    static const char *functionName = "findRecords";
    int chan;
    char recName[80];
    DBADDR addr;

    if (recordValue_) return asynSuccess;
    if (!prefix_) {
        printf("%s:%s: no records loaded, call testParallelCallbackLoadRecords() first\n",
               driverName, functionName);
        return asynError;
    }

    recordValue_ = (volatile epicsInt32 **)calloc(numChannels_,
                                                  sizeof(epicsInt32 *));
    for (chan = 0; chan < numChannels_; chan++) {
        epicsSnprintf(recName, sizeof(recName), "%s%d.VAL", prefix_, chan);
        if (dbNameToAddr(recName, &addr) != 0) {
            printf("%s:%s: record %s not found\n",
                   driverName, functionName, recName);
            free(recordValue_);
            recordValue_ = NULL;
            return asynError;
        }
        if (addr.field_type != DBF_LONG) {
            printf("%s:%s: record %s VAL is not DBF_LONG\n",
                   driverName, functionName, recName);
            free(recordValue_);
            recordValue_ = NULL;
            return asynError;
        }
        recordValue_[chan] = (volatile epicsInt32 *)addr.pfield;
    }
    return asynSuccess;
}

/** Accumulates the per round timings of bench() and queueBench() and prints
  * the summary in a form that can be compared between the two and across
  * worker thread counts. A round is split into
  *  - "push": the producer half, one entry posted per channel. In bench()
  *    this runs with the asyn port lock held.
  *  - "settle": from the end of the push until the last consumer has
  *    finished. This is the half that C worker threads should be able to
  *    work off C times faster.
  * The push half overlaps with the consumers, which start working as soon as
  * the first entry is posted, so a producer that is slowed down by the
  * consumers shows up as a longer push time. */
class roundStats {
public:
    roundStats()
      : rounds(0), pushSum(0.0), settleSum(0.0), totalSum(0.0),
        pushMax(0.0), settleMax(0.0) {}

    void add(const epicsTimeStamp *tStart, const epicsTimeStamp *tPushed,
             const epicsTimeStamp *tSettled)
    {
        double push = epicsTimeDiffInSeconds(tPushed, tStart);
        double settle = epicsTimeDiffInSeconds(tSettled, tPushed);
        rounds++;
        pushSum += push;
        settleSum += settle;
        totalSum += epicsTimeDiffInSeconds(tSettled, tStart);
        if (push > pushMax) pushMax = push;
        if (settle > settleMax) settleMax = settle;
    }

    void report(const char *pushLabel, const char *settleLabel) const
    {
        callbackQueueStats q;

        if (!rounds) return;
        printf("    %-28s mean %9.1f us   max %9.1f us\n",
               pushLabel, 1e6 * pushSum / rounds, 1e6 * pushMax);
        printf("    %-28s mean %9.1f us   max %9.1f us\n",
               settleLabel, 1e6 * settleSum / rounds, 1e6 * settleMax);
        printf("    %-28s mean %9.1f us\n",
               "round (push + settle)", 1e6 * totalSum / rounds);
        printf("    %-28s      %9.3f s\n", "total", totalSum);
        if (callbackQueueStatus(0, &q) == 0) {
            printf("    %-28s size %d, high water mark %d/%d/%d,"
                   " overflows %d/%d/%d\n", "callback queue (low/med/high)",
                   q.size, q.maxUsed[0], q.maxUsed[1], q.maxUsed[2],
                   q.numOverflow[0], q.numOverflow[1], q.numOverflow[2]);
        }
    }

private:
    int rounds;
    double pushSum, settleSum, totalSum;
    double pushMax, settleMax;
};

/** Runs numRounds closed loop rounds.
  *
  * Round r pushes the value r to every channel with a single
  * callParamCallbacks() and then waits until every record has arrived at r.
  * Because the next round is not pushed before the previous one has been
  * fully consumed, each round produces exactly one record processing per
  * channel: no value is dropped, no ring buffer overflows, and the per-round
  * time is the time the callback queue workers need to process numChannels
  * independent records. */
asynStatus testParallelCallback::bench(int numRounds, double timeout)
{
    static const char *functionName = "bench";
    int round, chan;
    epicsTimeStamp tStart, tPushed, tSettled, tNow;
    roundStats st;

    if (numRounds < 1) numRounds = 1;
    if (timeout <= 0.0) timeout = 10.0;
    if (findRecords() != asynSuccess) return asynError;

    /* Reset the high water marks so the report below covers this run only. */
    callbackQueueStatus(1, NULL);

    for (round = 1; round <= numRounds; round++) {
        epicsTimeGetCurrent(&tStart);
        lock();
        for (chan = 0; chan < numChannels_; chan++) {
            setIntegerParam(channelParam_[chan], round);
        }
        callParamCallbacks();
        unlock();
        epicsTimeGetCurrent(&tPushed);

        /* Wait for every record to have processed this round. Channels are
         * checked in order and the sweep resumes where it stopped, so a
         * completed round costs one pass over the channels. */
        chan = 0;
        while (chan < numChannels_) {
            if (*recordValue_[chan] == round) {
                chan++;
                continue;
            }
            epicsTimeGetCurrent(&tNow);
            if (epicsTimeDiffInSeconds(&tNow, &tPushed) > timeout) {
                printf("%s:%s: round %d timed out after %g s waiting for %s%d\n",
                       driverName, functionName, round, timeout, prefix_, chan);
                return asynError;
            }
            epicsThreadSleep(0.0);      /* yield, do not spin on a core */
        }
        epicsTimeGetCurrent(&tSettled);
        st.add(&tStart, &tPushed, &tSettled);
    }

    printf("%s:%s: port=%s channels=%d rounds=%d"
           " -> %d record processings\n",
           driverName, functionName, portName, numChannels_, numRounds,
           numChannels_ * numRounds);
    st.report("push (asyn port lock held)", "settle (record processing)");
    return asynSuccess;
}

/* Callback used by queueBench(): the cheapest thing a callback queue worker
 * can possibly do, so that what is measured is the queue, not the work. */
static void countingCallback(epicsCallback *pcallback)
{
    int *pcount = (int *)pcallback->user;
    epicsAtomicIncrIntT(pcount);
}

/** Control measurement for bench(): identical closed loop and identical
  * number of callback queue entries per round, but no asyn and no record
  * involved. Each round posts numChannels callbackRequest()s whose callback
  * only increments a counter, and waits until all of them have run.
  *
  * Whatever bench() and queueBench() have in common when the number of
  * callback queue worker threads is changed is a property of the EPICS
  * general purpose callback queue (a single mutex protected ring buffer plus
  * one epicsEventSignal() per entry, shared by all workers of a priority),
  * not of asyn. Only the difference between the two is asyn's own scaling
  * behaviour. */
asynStatus testParallelCallback::queueBench(int numRounds, double timeout,
                                            int priority)
{
    static const char *functionName = "queueBench";
    int round, i;
    int count = 0;
    epicsTimeStamp tStart, tPushed, tSettled, tNow;
    roundStats st;

    if (numRounds < 1) numRounds = 1;
    if (timeout <= 0.0) timeout = 10.0;
    if (priority < 0 || priority > 2) priority = priorityLow;

    if (!callbacks_) {
        callbacks_ = (epicsCallback *)calloc(numChannels_, sizeof(epicsCallback));
    }
    for (i = 0; i < numChannels_; i++) {
        callbackSetCallback(countingCallback, &callbacks_[i]);
        callbackSetPriority(priority, &callbacks_[i]);
        callbackSetUser(&count, &callbacks_[i]);
    }

    callbackQueueStatus(1, NULL);

    for (round = 1; round <= numRounds; round++) {
        epicsAtomicSetIntT(&count, 0);
        epicsTimeGetCurrent(&tStart);
        for (i = 0; i < numChannels_; i++) {
            callbackRequest(&callbacks_[i]);
        }
        epicsTimeGetCurrent(&tPushed);

        while (epicsAtomicGetIntT(&count) < numChannels_) {
            epicsTimeGetCurrent(&tNow);
            if (epicsTimeDiffInSeconds(&tNow, &tPushed) > timeout) {
                printf("%s:%s: round %d timed out after %g s, %d of %d callbacks ran\n",
                       driverName, functionName, round, timeout,
                       epicsAtomicGetIntT(&count), numChannels_);
                return asynError;
            }
            epicsThreadSleep(0.0);
        }
        epicsTimeGetCurrent(&tSettled);
        st.add(&tStart, &tPushed, &tSettled);
    }

    printf("%s:%s: (no asyn, no records) entries=%d rounds=%d"
           " -> %d callbacks\n",
           driverName, functionName, numChannels_, numRounds,
           numChannels_ * numRounds);
    st.report("post (callbackRequest)", "settle (callbacks run)");
    return asynSuccess;
}

extern "C" {

int testParallelCallbackConfigure(const char *portName, int numChannels)
{
    new testParallelCallback(portName, numChannels);
    return asynSuccess;
}

int testParallelCallbackLoadRecords(const char *portName, const char *dbFile,
                                    const char *macros, const char *prefix)
{
    testParallelCallback *pDriver =
        findDerivedAsynPortDriver<testParallelCallback>(portName);
    if (!pDriver) {
        printf("testParallelCallbackLoadRecords: port %s not found\n", portName);
        return asynError;
    }
    return (pDriver->loadRecords(dbFile, macros, prefix) < 0) ? asynError
                                                              : asynSuccess;
}

int testParallelCallbackBench(const char *portName, int numRounds, double timeout)
{
    testParallelCallback *pDriver =
        findDerivedAsynPortDriver<testParallelCallback>(portName);
    if (!pDriver) {
        printf("testParallelCallbackBench: port %s not found\n", portName);
        return asynError;
    }
    return pDriver->bench(numRounds, timeout);
}

int testParallelCallbackQueueBench(const char *portName, int numRounds,
                                   double timeout, int priority)
{
    testParallelCallback *pDriver =
        findDerivedAsynPortDriver<testParallelCallback>(portName);
    if (!pDriver) {
        printf("testParallelCallbackQueueBench: port %s not found\n", portName);
        return asynError;
    }
    return pDriver->queueBench(numRounds, timeout, priority);
}

/* EPICS iocsh shell commands */

static const iocshArg configureArg0 = { "portName", iocshArgString };
static const iocshArg configureArg1 = { "numChannels", iocshArgInt };
static const iocshArg * const configureArgs[] = { &configureArg0, &configureArg1 };
#ifdef IOCSHFUNCDEF_HAS_USAGE
static const char configureUsage[] =
    "Create a testParallelCallback asyn port with numChannels asynInt32 parameters\n";
#endif
static const iocshFuncDef configureFuncDef = {
    "testParallelCallbackConfigure", 2, configureArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
    , configureUsage
#endif
};
static void configureCallFunc(const iocshArgBuf *args)
{
    testParallelCallbackConfigure(args[0].sval, args[1].ival);
}

static const iocshArg loadArg0 = { "portName", iocshArgString };
static const iocshArg loadArg1 = { "dbFile", iocshArgString };
static const iocshArg loadArg2 = { "macros", iocshArgString };
static const iocshArg loadArg3 = { "recordPrefix", iocshArgString };
static const iocshArg * const loadArgs[] = { &loadArg0, &loadArg1, &loadArg2,
                                             &loadArg3 };
#ifdef IOCSHFUNCDEF_HAS_USAGE
static const char loadUsage[] =
    "Load dbFile once per channel of portName, with macros plus P=recordPrefix\n"
    "and N=<channel number>, creating records recordPrefix0 .. recordPrefixN-1\n";
#endif
static const iocshFuncDef loadFuncDef = {
    "testParallelCallbackLoadRecords", 4, loadArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
    , loadUsage
#endif
};
static void loadCallFunc(const iocshArgBuf *args)
{
    testParallelCallbackLoadRecords(args[0].sval, args[1].sval, args[2].sval,
                                    args[3].sval);
}

static const iocshArg benchArg0 = { "portName", iocshArgString };
static const iocshArg benchArg1 = { "numRounds", iocshArgInt };
static const iocshArg benchArg2 = { "timeout", iocshArgDouble };
static const iocshArg * const benchArgs[] = { &benchArg0, &benchArg1, &benchArg2 };
#ifdef IOCSHFUNCDEF_HAS_USAGE
static const char benchUsage[] =
    "Push numRounds rounds of one new value per channel to portName, waiting\n"
    "for every record to process each round before pushing the next one, and\n"
    "print how long the rounds took\n";
#endif
static const iocshFuncDef benchFuncDef = {
    "testParallelCallbackBench", 3, benchArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
    , benchUsage
#endif
};
static void benchCallFunc(const iocshArgBuf *args)
{
    testParallelCallbackBench(args[0].sval, args[1].ival, args[2].dval);
}

static const iocshArg queueBenchArg0 = { "portName", iocshArgString };
static const iocshArg queueBenchArg1 = { "numRounds", iocshArgInt };
static const iocshArg queueBenchArg2 = { "timeout", iocshArgDouble };
static const iocshArg queueBenchArg3 = { "priority (0=LOW, 1=MEDIUM, 2=HIGH)",
                                         iocshArgInt };
static const iocshArg * const queueBenchArgs[] = { &queueBenchArg0,
                                                   &queueBenchArg1,
                                                   &queueBenchArg2,
                                                   &queueBenchArg3 };
#ifdef IOCSHFUNCDEF_HAS_USAGE
static const char queueBenchUsage[] =
    "Control measurement for testParallelCallbackBench(): the same closed\n"
    "loop with the same number of callback queue entries per round, but the\n"
    "entries are plain counting callbackRequest()s, with no asyn port and no\n"
    "record involved\n";
#endif
static const iocshFuncDef queueBenchFuncDef = {
    "testParallelCallbackQueueBench", 4, queueBenchArgs
#ifdef IOCSHFUNCDEF_HAS_USAGE
    , queueBenchUsage
#endif
};
static void queueBenchCallFunc(const iocshArgBuf *args)
{
    testParallelCallbackQueueBench(args[0].sval, args[1].ival, args[2].dval,
                                   args[3].ival);
}

void testParallelCallbackRegister(void)
{
    iocshRegister(&configureFuncDef, configureCallFunc);
    iocshRegister(&loadFuncDef, loadCallFunc);
    iocshRegister(&benchFuncDef, benchCallFunc);
    iocshRegister(&queueBenchFuncDef, queueBenchCallFunc);
}

epicsExportRegistrar(testParallelCallbackRegister);

}
