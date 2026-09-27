#!../../bin/linux-x86_64/testParallelCallback
#
# Measures how asyn's generic device support scales when the EPICS general
# purpose callback queue is served by more than one worker thread
# (https://github.com/epics-modules/asyn/issues/170).
#
# NCHAN independent longin records, all SCAN = "I/O Intr" on one asyn port and
# all at the same scan priority, are fed one new value each per round. With
# NTHREADS workers on that priority, NTHREADS records should be processed
# concurrently, so the "settle" time printed per run should fall as NTHREADS
# rises. Run the same benchmark for several NTHREADS values and compare -
# runBenchmark.sh does that and prints the table.
#
# Requires EPICS Base 7.
#
# Environment variables (all optional):
#   NCHAN     number of channels/records            (default 500)
#   NTHREADS  callback queue workers for cbLOW      (default 1)
#   ROUNDS    number of benchmark rounds            (default 200)
#   QSIZE     callback queue size                   (default 20000)
#   PRIO      scan priority of the records          (default LOW)
#   FIFO      per record ring buffer depth          (default 10)

dbLoadDatabase("../../dbd/testParallelCallback.dbd")
testParallelCallback_registerRecordDeviceDriver(pdbbase)

epicsEnvSet("NCHAN",    "$(NCHAN=500)")
epicsEnvSet("NTHREADS", "$(NTHREADS=1)")
epicsEnvSet("ROUNDS",   "$(ROUNDS=200)")
epicsEnvSet("QSIZE",    "$(QSIZE=20000)")
epicsEnvSet("PRIO",     "$(PRIO=LOW)")
epicsEnvSet("FIFO",     "$(FIFO=10)")

# Both must be called before iocInit().
callbackSetQueueSize($(QSIZE))
callbackParallelThreads($(NTHREADS), "$(PRIO)")

testParallelCallbackConfigure("PORT1", $(NCHAN))

# One record per channel, named PORT1Chan0 .. PORT1Chan<NCHAN-1>.
testParallelCallbackLoadRecords("PORT1", "../../db/testParallelCallbackChan.db", "PORT=PORT1,PRIO=$(PRIO),FIFO=$(FIFO)", "testParallelCallback:Chan")

iocInit()

# Warm up (first processing of each record, lock set creation, page faults)
# before the runs that are reported.
testParallelCallbackBench("PORT1", 20, 10)
testParallelCallbackQueueBench("PORT1", 20, 10, 0)

echo "=== NCHAN=$(NCHAN) NTHREADS=$(NTHREADS) PRIO=$(PRIO) ROUNDS=$(ROUNDS) ==="

# 1) asyn: one interrupt callback per channel per round, each turned into one
#    process request for one I/O Intr record by devAsynInt32.
testParallelCallbackBench("PORT1", $(ROUNDS), 10)

# 2) Control: the same number of callback queue entries per round, but plain
#    counting callbacks - no asyn port, no record, no lock set. Whatever this
#    measurement does when NTHREADS changes is the EPICS callback queue's own
#    scaling behaviour and cannot be fixed in asyn; only the difference
#    between 1) and 2) is asyn's.
testParallelCallbackQueueBench("PORT1", $(ROUNDS), 10, 0)
