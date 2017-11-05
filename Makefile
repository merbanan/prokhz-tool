CC = gcc


all: libprokhz prokhz-tool


libprokhz:
	$(CC) -Wall -Og -g3 -c drivers/ctx_idrw_203.c -o drivers/ctx_idrw_203.o
	$(CC) -Wall -Og -g3 -c libprokhz.c -o libprokhz.o
	ar rcs libprokhz.a libprokhz.o drivers/*.o

prokhz-tool:
	$(CC) -Wall -Og -g3 -static prokhz-tool.c -L. -lprokhz -o prokhz-tool

clean:
	rm -f *.o prokhz-tool drivers/*.o

install:
	cp 20-rwrfid.rules /etc/udev/rules.d/
