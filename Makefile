CC=gcc
CFLAGS=-pedantic `pkg-config --cflags mpv` -shared -fPIC -Wall -Wvla
LIBS=
SRC=limited_autoload.c

build: $(SRC)
	$(CC) $(CFLAGS) -O2 -D "DEBUG=0" -o limited_autoload.so $(SRC) $(LIBS) 

debug: $(SRC)
	$(CC) $(CFLAGS) -D "DEBUG=1" -o limited_autoload.so $(SRC) $(LIBS)
