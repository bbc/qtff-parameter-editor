CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE

.PHONY: all
all: rdd36dump rdd36mod movdump

rdd36dump: rdd36dump.o
	g++ $< -o $@

rdd36dump.o: rdd36dump.c
	g++   -c ${CFLAGS} $< -o $@

rdd36mod: rdd36mod.o
	g++ $< -o $@

rdd36mod.o: rdd36mod.c
	g++ -c ${CFLAGS} $< -o $@
    
movdump: movdump.o
	g++ $< -o $@

movdump.o: movdump.cpp
	g++ -c ${CFLAGS} $< -o $@

.PHONY: clean
clean:
	@rm -f rdd36dump.o rdd36mod.o movdump.o rdd36dump rdd36mod movdump
    