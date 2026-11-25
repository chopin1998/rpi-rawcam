CXX      := g++
CXXFLAGS := -O3 -march=armv8-a -std=c++17 -Wall -Wextra -pthread
TARGET   := bin/rawcamd
SRCS     := src/main.cpp src/camera.cpp
HDRS     := src/camera.hpp src/h264_enc.hpp src/i2c_ctrl.hpp src/protocol.hpp

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
