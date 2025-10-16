
# chugin name
CHUGIN_NAME=DMX

# all of the c/cpp files that compose this chugin
C_MODULES=
CXX_MODULES=DMX.cpp

# where to find chugin.h
CK_SRC_PATH?=chuck/include

# where to install chugin
CHUGIN_PATH?=/usr/local/lib/chuck

PROJECT_BASE?=$(shell pwd)
# Common include directories
COMMON_INCLUDES=-I$(CK_SRC_PATH) \
				-I$(PROJECT_BASE)/libartnet/include \
				-I$(PROJECT_BASE)/serial/include \
				-I$(PROJECT_BASE)/sACN/include

# Library search paths only (no -l here)
COMMON_LIB_PATHS=-L$(PROJECT_BASE)/libartnet/msvc/libartnet/x64/Release \
				 -L$(PROJECT_BASE)/sACN/lib

# Platform-dependent libs
LINUX_LIBS=-lartnet -lsacn -letcpal -lpthread -lstdc++
MAC_LIBS=-lartnet -lsacn -letcpal -lstdc++
WIN_SYSTEM_LIBS=-lWs2_32 -lMswsock -lOle32 -lUser32 -lAdvapi32 -lKernel32 -lRpcrt4 -lWinmm -lIphlpapi -lmingw32 -lgcc -lmsvcrt -lmingwex
WIN_LIBS=-lartnet -lsacn -letcpal $(WIN_SYSTEM_LIBS)

# ---------------------------------------------------------------------------- #
# you won't generally need to change anything below this line for a new chugin #
# ---------------------------------------------------------------------------- #

# default target: print usage message and quit
current: 
	@echo "[chugin build]: please use one of the following configurations:"
	@echo "   make linux, make mac, make web, or make win32"

ifneq ($(CK_TARGET),)
.DEFAULT_GOAL:=$(CK_TARGET)
ifeq ($(MAKECMDGOALS),)
MAKECMDGOALS:=$(.DEFAULT_GOAL)
endif
endif

.PHONY: mac osx linux linux-pulse linux-oss linux-jack linux-alsa win32
mac osx linux linux-pulse linux-oss linux-jack linux-alsa win32: all

CC=gcc
CXX=gcc
LD=g++

ifneq (,$(filter linux linux-pulse linux-oss linux-jack linux-alsa,$(MAKECMDGOALS)))
	FLAGS += $(COMMON_INCLUDES) -fPIC
	CXXFLAGS += $(FLAGS)
	LDFLAGS := $(COMMON_LIB_PATHS)
	LDFLAGS_LIBS := $(LINUX_LIBS)
	include makefile.linux
endif

ifneq (,$(filter mac osx,$(MAKECMDGOALS)))
	FLAGS += $(COMMON_INCLUDES) -fPIC
	CXXFLAGS += $(FLAGS)
	LDFLAGS := $(COMMON_LIB_PATHS)
	LDFLAGS_LIBS := $(MAC_LIBS)
	include makefile.mac
endif

ifneq (,$(filter win32,$(MAKECMDGOALS)))
	FLAGS += $(COMMON_INCLUDES) -fno-stack-protector
	CXXFLAGS += $(FLAGS)
	LDFLAGS := $(COMMON_LIB_PATHS)
	LDFLAGS_LIBS := $(WIN_LIBS)
	include makefile.win
endif

ifneq ($(CHUCK_DEBUG),)
FLAGS+= -g
else
FLAGS+= -O3
endif

ifneq ($(CHUCK_STRICT),)
FLAGS+= -Werror
endif

# default: build a dynamic chugin
CK_CHUGIN_STATIC?=0

ifeq ($(CK_CHUGIN_STATIC),0)
SUFFIX=.chug
else
SUFFIX=.schug
FLAGS+= -D__CK_DLL_STATIC__
endif
# webchugin extension
WEBSUFFIX=.wasm

C_OBJECTS=$(addsuffix .o,$(basename $(C_MODULES)))
CXX_OBJECTS=$(addsuffix .o,$(basename $(CXX_MODULES)))

CHUG=$(addsuffix $(SUFFIX),$(CHUGIN_NAME))
WEBCHUG=$(addsuffix $(WEBSUFFIX),$(CHUG))

all: $(CHUG)

$(CHUG): $(C_OBJECTS) $(CXX_OBJECTS)
ifeq ($(CK_CHUGIN_STATIC),0)
	$(LD) -o $@ $^ $(LDFLAGS) $(LDFLAGS_LIBS)
else
	ar rv $@ $^
	ranlib $@
endif

$(C_OBJECTS): %.o: %.c
	$(CC) $(FLAGS) -c -o $@ $<

$(CXX_OBJECTS): %.o: %.cpp $(CK_SRC_PATH)/chugin.h
	$(CXX) $(FLAGS) -c -o $@ $<

# build as webchugin
web:
	emcc -O3 -s SIDE_MODULE=1 -s DISABLE_EXCEPTION_CATCHING=0 -fPIC -Wformat=0 	-I $(CK_SRC_PATH) $(CXX_MODULES) $(C_MODULES) -o $(WEBCHUG)

install: $(CHUG)
	mkdir -p $(CHUGIN_PATH)
	cp $^ $(CHUGIN_PATH)
	chmod 755 $(CHUGIN_PATH)/$(CHUG)

clean: 
	rm -rf $(C_OBJECTS) $(CXX_OBJECTS) $(CHUG) $(WEBCHUG) Release Debug
