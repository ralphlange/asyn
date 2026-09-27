/*
 * testParallelCallback.h
 *
 * Asyn port driver used to measure how well asyn's generic device support
 * scales when the EPICS general purpose callback queue is served by more than
 * one worker thread (callbackParallelThreads(), see callback.c in EPICS Base).
 *
 * The scenario is the one reported in
 * https://github.com/epics-modules/asyn/issues/170: a port with many channels,
 * each feeding one I/O Intr scanned record, all records independent (no links,
 * therefore one lock set per record, so record processing itself is trivially
 * parallel). Every round of updates delivers one new value to every channel,
 * which devAsynInt32 turns into one scanIoRequest() per record. With C worker
 * threads serving the queue, C records should be processed concurrently and
 * the time to work off one round should fall roughly as 1/C.
 *
 * bench() measures that directly, with a closed loop: it pushes round r to
 * every channel and then waits until every record has arrived at r before
 * pushing r+1. No value is ever dropped and no ring buffer ever overflows, so
 * the number of record processings per round is exactly the number of channels
 * and the measured per-round time is a clean throughput figure that can be
 * compared across different worker thread counts.
 *
 * Requires EPICS Base 7: callbackQueueStatus() and the epicsCallback type are
 * not available in 3.14 or 3.15, so the top level Makefile only descends into
 * this application when BASE_7_0 is set.
 *
 * Author: Ralph Lange
 */

#ifndef testParallelCallback_H
#define testParallelCallback_H

#include <callback.h>
#include <dbAccess.h>

#include "asynPortDriver.h"

/** Asyn port driver with a configurable number of asynInt32 parameters
  * ("CHANNEL0" .. "CHANNELn-1"), each usable as the drvInfo for one
  * I/O Intr scanned record. */
class testParallelCallback : public asynPortDriver {
public:
    testParallelCallback(const char *portName, int numChannels);
    ~testParallelCallback();

    int numChannels() const { return numChannels_; }

    /** Load one record per channel from dbFile, appending "P=<prefix>,N=<n>"
      * to macros, and remember prefix so that bench() can find the records
      * again. Returns the number of records loaded, or -1 on error. */
    int loadRecords(const char *dbFile, const char *macros, const char *prefix);

    /** Run numRounds closed loop rounds and print the timing summary.
      * timeout is the number of seconds to wait for a single round to be
      * worked off before giving up. */
    asynStatus bench(int numRounds, double timeout);

    /** Control measurement that does not involve asyn or the record layer at
      * all: the same closed loop, but each round posts one callbackRequest()
      * per channel whose callback only counts. Anything the two measurements
      * have in common is the behaviour of the EPICS general purpose callback
      * queue itself, not of asyn. */
    asynStatus queueBench(int numRounds, double timeout, int priority);

private:
    /** Resolve the VAL field of the record belonging to every channel.
      * Done once, on the first bench() call. */
    asynStatus findRecords();

    int numChannels_;
    int *channelParam_;
    char *prefix_;
    volatile epicsInt32 **recordValue_;
    epicsCallback *callbacks_;
};

#endif
