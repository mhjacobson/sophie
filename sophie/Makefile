sophie: sophie.cpp output.cpp output.h input.cpp input.h util.h
	clang -o sophie sophie.cpp input.cpp output.cpp -isystem ./ffmpeg-4.4.2/install/usr/local/include/ -L./ffmpeg-4.4.2/install/usr/local/lib/ -L/usr/local/lib/ -lavformat -lavcodec -lavutil -lc++ -lpng -g

sophie-system: sophie.cpp output.cpp output.h input.cpp input.h util.h
	clang -o sophie sophie.cpp input.cpp output.cpp -isystem /usr/local/include/ -L/usr/local/lib/ -lavformat -lavcodec -lavutil -lpng -lc++ -g -Os

sophie-static: sophie.cpp output.cpp output.h input.cpp input.h util.h
	clang -o sophie sophie.cpp input.cpp output.cpp -isystem ./ffmpeg-4.4.2/install/usr/local/include/ -L./ffmpeg-4.4.2/install/usr/local/lib/ -L/usr/local/lib/ -lavformat -lavcodec -lavutil -lswresample -lc++ -lm -lz -lx264 -lbz2 -lpthread -llzma -lva -lva-drm -lssl -lcrypto -lpng -g
