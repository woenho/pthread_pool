# * ---------------------
# * UTF-8 한글확인 용
# * ---------------------
 
#.SUFFIXES = .cpp .o
CXX=g++

LIBATP = libAsyncThreadPool.a
SRCS = AsyncThreadPool.cpp 
OBJS = $(SRCS:.cpp=.o)

CFLAGS_RELEASE = -O2 -fPIC
CFLAGS_DEBUG = -O0 -g -ggdb3 -DDEBUG
CFLAGS  = -Wall -Wextra -Wshadow -Wformat-security -Winit-self -fpermissive
CFLAGS += -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function

LFLAGS  = -lm -ldl -lpthread

ifeq (release, $(findstring release, $(MAKECMDGOALS))) #"make clean release"으로 clean 다음에 연이어 release가 나올 수가 있다.
	CFLAGS += $(CFLAGS_RELEASE)
else
	CFLAGS += $(CFLAGS_DEBUG)
	LIBATP = libAsyncThreadPool_d.a
ifeq (debugtrace, $(findstring debugtrace, $(MAKECMDGOALS))) #"make clean debugtrace"으로 clean 다음에 연이어 debugtrace 나올 수가 있다.
	CFLAGS += -DDEBUGTRACE
endif
endif

.PHONY: all clean debug release debugtrace cversion

all: cversion $(LIBATP) example
debug: all
release: all
debugtrace: all

%.o: %.cpp
	$(CXX) -o $@ $< -c $(CFLAGS)

cversion:
	echo `$(CXX) -v`

$(LIBATP): $(OBJS)
	ar cq $(LIBATP) $(OBJS)
	mkdir -p ../lib
	cp $(LIBATP) ../lib
	mkdir -p ../include
	cp AsyncThreadPool.h ../include

example: ExampleThreadPool.o
	$(CXX) -o $@ $< $(LIBATP) $(LFLAGS)

clean:
	rm -f $(LIBATP) example *.o


