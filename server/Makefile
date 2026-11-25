CC      := gcc
CFLAGS  := -O3 -march=armv8-a -std=c11 -D_GNU_SOURCE -Wall -Wextra -pthread
LDFLAGS := -pthread -lm
TARGET  := bin/rawcamd
SRCS    := src/main.c src/camera.c src/h264_enc.c src/i2c_ctrl.c
HDRS    := src/camera.h src/h264_enc.h src/i2c_ctrl.h src/protocol.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
