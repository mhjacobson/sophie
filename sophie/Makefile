CC=clang
OTHER_CFLAGS=
LIBS=-lavformat -lavcodec -lavutil -lswscale -lc++ -lpng

.PHONY: clean

sophie: sophie.cpp output.cpp output.h input.cpp input.h util.h
	${CC} -o "$@" sophie.cpp output.cpp input.cpp --std=c++17 -Os -g ${OTHER_CFLAGS} -isystem /usr/local/include -L /usr/local/lib ${LIBS}

clean:
	rm sophie
