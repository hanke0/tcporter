CFLAGS=-Wall -pedantic -pthread -D _GNU_SOURCE
PROGRAM=tcporter

debug:
	$(CC) $(CFLAGS) main.c -o $(PROGRAM)-debug
release:
	$(CC) $(CFLAGS) main.c -O2 -o $(PROGRAM)
clean:
	$(RM) -f $(PROGRAM) $(PROGRAM)-debug 
