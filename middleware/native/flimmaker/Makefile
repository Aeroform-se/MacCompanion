CXX ?= g++
VERSION ?= macflim-netflim
CXXFLAGS += -O3 -std=c++20 -Wall -Wextra -Wno-deprecated-declarations -DVERSION=\"$(VERSION)\"
LDLIBS += -lavformat -lavcodec -lavutil

OBJS = grayscale.o writer.o ruler.o watermark.o imgcompress.o \
       filesystem_reader.o ffmpeg_reader.o flimmaker.o common.o \
       flim.o subtitles.o flimutil.o dithering_parameters.o \
       flimcompressor.o compressor_helper.o encode_frame.o decoder.o \
       profile.o flimencoder.o cmdline.o

TARGET = flimmaker

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
