# testParallelCallback

Measures how I/O Intr scanned asyn records scale when the EPICS general
purpose callback queue is served by more than one worker thread, i.e. the
second half of [issue #170](https://github.com/epics-modules/asyn/issues/170):

> ASYN is not ready for callback parallelization. [...] At ITER we have seen
> ASYN port driver performance going noticeably down when parallel callback
> threads are enabled.

The first half of that issue - a record occupying more than one callback queue
entry at a time - is covered by `testQueueOverflowApp`.

## What is measured

`testParallelCallback` is an asyn port driver with a configurable number of
`asynInt32` parameters ("CHANNEL0" .. "CHANNELn-1"), each feeding exactly one
I/O Intr scanned `longin` record. The records have **no links at all**, so
every record sits in its own lock set and nothing in the record layer prevents
all of them from being processed at the same time.

`testParallelCallbackBench()` runs a **closed loop**: it pushes round `r` to
every channel with a single `callParamCallbacks()` and then waits until every
record has arrived at `r` before pushing `r+1`. Because a round is never
started before the previous one has been fully consumed, each round produces
exactly one record processing per channel - nothing is dropped, no ring buffer
ever overflows - so the per-round time is a clean throughput figure that can be
compared across worker thread counts. A round is reported in two halves:

- **push** - the producer half: the driver delivers one interrupt callback per
  channel, holding the asyn port lock. `devAsynInt32` turns each one into one
  `scanIoRequest()`, i.e. one entry on the `cbLow` queue.
- **settle** - from the end of the push until the last record has processed.

The consumers start working as soon as the first entry is posted, so the two
halves overlap: a producer that is held up by the consumers shows up as a
longer **push** time, not as a longer settle time.

`testParallelCallbackQueueBench()` is the **control**. It runs the identical
closed loop, posting the identical number of callback queue entries per round,
but the entries are plain `callbackRequest()`s whose callback only increments a
counter - no asyn port, no record, no lock set, no ring buffer. Anything the
two measurements do in common when the worker thread count changes is a
property of the EPICS callback queue and cannot be fixed inside asyn.

## Building

**Requires EPICS Base 7.** The application uses `callbackQueueStatus()` and the
`epicsCallback` type, neither of which exists in 3.14 or 3.15, so the top level
Makefile leaves `testParallelCallbackApp` out unless `BASE_7_0` is set.

```
make -C testParallelCallbackApp
make -C iocBoot/ioctestParallelCallback
```

## Running

```
cd iocBoot/ioctestParallelCallback
./runBenchmark.sh [channels [rounds [threads ...]]]     # default: 500 200 1 2 4 8
```

`callbackParallelThreads()` only has an effect before `iocInit()`, so the
script starts one IOC per thread count and collects the numbers into a table.
A single run can also be done by hand, with `NCHAN`, `NTHREADS`, `ROUNDS`,
`QSIZE`, `PRIO` and `FIFO` in the environment:

```
NCHAN=1000 NTHREADS=4 ROUNDS=100 ../../bin/<EPICS_HOST_ARCH>/testParallelCallback st.cmd
```

## What to look for

1000 channels, 100 rounds, i.e. 100000 record processings per column, on an
idle 12 core x86_64 Linux box, EPICS Base 7.0.10, microseconds per round:

```
          |        asyn I/O Intr records |    plain callbacks (control)
   cb thr |     push   settle      round |     push   settle      round
----------|------------------------------|-----------------------------
        1 |   6824.5     64.6     6889.1 |   1605.6     78.2     1683.9
        2 |  23568.8     24.1    23592.9 |  35565.3    375.4    35940.8
        4 |  27834.2     26.5    27860.8 |  50950.7    500.1    51450.8
        8 |  24865.2     19.7    24884.9 |  53915.7    265.0    54180.7
```

**The reported effect reproduces.** Going from one callback worker thread to
two makes 1000 independent, unlinked records with a 1 Hz-style update pattern
**3.4 times slower**, and adding more threads does not win any of it back.
There is nothing to serialize the records themselves: at one thread the whole
round costs 6.9 us per record, so a second worker ought to have halved it.

**The slowdown is not in asyn.** The control column, which never touches an
asyn port or a record, degrades *more* steeply - 1.7 ms to 54 ms, a factor of
32. Whatever asyn does or does not do, posting N entries on a callback queue
that is being drained by more than one worker is what costs the time. (The
control degrades more than asyn only because its producer does no work
whatsoever between two entries, which maximises the contention; asyn's
producer spends about 5 us of its own per channel, which throttles it.)

**The producer is blocking, not spinning.** Voluntary context switches of the
producer thread, sampled over 3 s from `/proc/<pid>/status` while the asyn
benchmark runs:

```
  threads   voluntary_ctxt_switches/s
        1          76
        2       13907
        4       10088
        8        5842
```

With a single worker the producer never blocks. With two, it blocks roughly
every third `scanIoRequest()`. `callbackRequest()` does two things per entry:
`epicsRingPointerPush()` on a ring created with `epicsRingPointerLockedCreate()`
- one mutex shared by the producer and every worker of that priority - and an
`epicsEventSignal()`. Workers hammer that same mutex on every pop, so the
producer is put to sleep on it over and over.

**Consequences for asyn.** The only lever asyn has over this is *how many
queue entries it posts*, which is exactly what the `devAsynXXX` change on this
branch addresses: one outstanding process request per record instead of one
per buffered value. Whether more than one worker thread per priority can ever
pay off for I/O Intr scanned records is a question about
`callbackRequest()`/`epicsRingPointer` in EPICS Base, not about asyn, and
should be raised there.

## Caveats

- The numbers above are from one machine; the shape of the effect matters, not
  the absolute values. Reproduce on your own hardware before drawing
  conclusions - the cost of a contended futex varies a lot between kernels and
  between bare metal, containers and VMs.
- The benchmark polls each record's `VAL` field without taking the record lock.
  Taking `dbScanLock()` per poll would compete with the very worker threads
  whose scaling is being measured. A stale read can only ever make a round look
  longer, never shorter.
- `bench()` leaves the records at the last round number, so a run can be
  verified afterwards with `dbgf testParallelCallback:Chan0`.
