TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

# dbAccess.h is included for dbLoadRecords()/dbNameToAddr()
USR_CPPFLAGS += -DUSE_TYPED_RSET -DUSE_TYPED_DSET

DBD += testParallelCallback.dbd

LIBRARY_IOC += testParallelCallbackSupport
testParallelCallbackSupport_SRCS += testParallelCallback.cpp
testParallelCallbackSupport_LIBS += asyn
testParallelCallbackSupport_LIBS += $(EPICS_BASE_IOC_LIBS)

#=============================

PROD_IOC += testParallelCallback

# <name>_registerRecordDeviceDriver.cpp will be created from <name>.dbd
testParallelCallback_SRCS_DEFAULT += testParallelCallback_registerRecordDeviceDriver.cpp testParallelCallbackMain.cpp
testParallelCallback_SRCS_vxWorks  += testParallelCallback_registerRecordDeviceDriver.cpp
testParallelCallback_LIBS += testParallelCallbackSupport asyn
testParallelCallback_LIBS += $(EPICS_BASE_IOC_LIBS)

testParallelCallback_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE
