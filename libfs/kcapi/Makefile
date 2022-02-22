all: kcapilib

CODE=.
CXX=g++
CC=gcc

obj-m := libkcapilib.o
KDIR := /lib/modules/`uname -r`/build
PWD := `pwd`

kcapilib:
	gcc -fPIC -shared -o libkcapilib.so kcapi-sym.c kcapi-kernel-if.c


install:
	sudo cp *.so /usr/local/lib
	sudo cp *.so /usr/lib
	sudo cp *.h /usr/local/include
	sudo cp *.h /usr/include
	sudo cp *.h /usr/lib/

clean:
	rm -rf *.o
	rm -rf *.so
	rm -rf *.so
