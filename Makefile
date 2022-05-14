CFLAGS := -Wall -pedantic -pthread -m64 -std=c11
PROGRAM := tcporter
TARGET := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifeq ($(TARGET), Linux)
	CFLAGS += -D_GNU_SOURCE
endif

all: release

.PHONY: debug
debug:
	$(CC) $(CFLAGS) main.c -O0 -g -o $(PROGRAM)-debug
.PHONY: release
release:
	$(CC) $(CFLAGS) main.c -O2 -DNDEBUG -o $(PROGRAM)
.PHONY: clean
clean:
	$(RM) -f $(PROGRAM) $(PROGRAM)-debug 
.PHONY: lint
lint:
	clang-format -i main.c
.PHONY: buildinfo
buildinfo:
	@echo target: $(TARGET)
