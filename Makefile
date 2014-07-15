buse.o: buse.c
	gcc -c buse.c -o buse.o
ploop.o: ploop.cpp
	g++ -c ploop.cpp -o ploop.o
ploop_userspace: ploop.o buse.o
        g++ ploop.o buse.o -o ploop_userspace
clean:
	rm -f buse.o
