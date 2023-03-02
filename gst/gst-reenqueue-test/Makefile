CXXFLAGS+=`pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0` -std=c++14
LDFLAGS+=`pkg-config --libs gstreamer-1.0 gstreamer-app-1.0`

all: gst-reenqueue-test

gst-reenqueue-test: Makefile test_src.cpp gst-reenqueue-test.cpp
	$(CXX) -std=c++14 -O0 -g3 $(CXXFLAGS) test_src.cpp gst-reenqueue-test.cpp $(LDFLAGS) -o gst-reenqueue-test

clean:
	rm -f gst-reenqueue-test
