TOP = ..
include $(TOP)/configure/CONFIG
# This application uses callbackQueueStatus() and the epicsCallback type,
# neither of which is available before EPICS Base 7. The top level Makefile
# leaves the directory out on older versions; the guard here is repeated so
# that a direct "make -C testParallelCallbackApp" does not fail either.
ifdef BASE_7_0
DIRS += src
DIRS += Db
endif
include $(TOP)/configure/RULES_DIRS
