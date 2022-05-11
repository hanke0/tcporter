CFLAGS=-Wall -pedantic -pthread
PROGRAM=tcporter

all: release

debug:
	$(CC) $(CFLAGS) main.c -O0 -g -o $(PROGRAM)-debug
release:
	$(CC) $(CFLAGS) main.c -O2 -o $(PROGRAM)
clean:
	$(RM) -f $(PROGRAM) $(PROGRAM)-debug 
lint:
	clang-format -i main.c
