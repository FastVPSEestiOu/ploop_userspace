buse.o: buse.c
	gcc -c buse.c -o buse.o
ploop_userspace:
	g++ ploop.cpp buse.o
clean:
	rm -f buse.o
