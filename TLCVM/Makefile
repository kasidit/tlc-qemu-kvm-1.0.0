all: memory-server-block memory-server-interleave restore-block restore-interleave

restore-interleave: restore-interleave.o
	gcc restore-interleave.o -o restore-interleave -lpthread

restore-interleave.o: restore-interleave.c
	gcc -c restore-interleave.c 

restore-block: restore-block.o
	gcc restore-block.o -o restore-block -lpthread

restore-block.o: restore-block.c
	gcc -c restore-block.c 

memory-server-interleave: memory-server-interleave.o
	gcc memory-server-interleave.o -o memory-server-interleave

memory-server-interleave.o: memory-server-interleave.c
	gcc -c memory-server-interleave.c 

memory-server-block: memory-server-block.o 
	gcc memory-server-block.o -o memory-server-block

memory-server-block.o: memory-server-block.c
	gcc -c memory-server-block.c 

clean:
	rm -rf *.o memory-server-block memory-server-interleave restore-interleave restore-block
