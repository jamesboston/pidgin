all:
	$(CC) -fPIC  -O2 -g -Wall -o sluggish.so -shared sluggish.c \
	    `pkg-config --cflags --libs purple-3` -DUSE_SCREENSAVER
