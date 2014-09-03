all: memory-server-block memory-server-interleave

memory-server-interleave: memory-server-interleave.o
	gcc memory-server-interleave.o -o memory-server-interleave

memory-server-interleave.o: memory-server-interleave.c
	gcc -c memory-server-interleave.c 

memory-server-block: memory-server-block.o memory.o
	gcc memory-server-block.o memory.o -o memory-server-block

memory-server-block.o: memory-server-block.c
	gcc -c memory-server-block.c 

memory.o: memory.c
	gcc -c memory.c 

clean:
	rm -rf *.o memory-server-block memory-server-interleave
