TOP = ../..
include $(TOP)/configure/CONFIG
ARCH = linux-x86_64
# testParallelCallbackApp needs EPICS Base 7, so there is no IOC to boot here
# on older versions
ifdef BASE_7_0
TARGETS = envPaths cdCommands dllPath.bat
endif
include $(TOP)/configure/RULES.ioc
