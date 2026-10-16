# Builds the C and C++ examples against the Rust static library.   make -C examples
ROOT  := ..
LIB   := $(ROOT)/target/release/libmcscon.a
OUT   := build
LDLIBS := $(LIB) -lm -lpthread -ldl

all: $(OUT)/bot_c $(OUT)/bot_cpp

$(LIB): $(wildcard $(ROOT)/src/*.rs $(ROOT)/src/packets/*.rs)
	cd $(ROOT) && cargo rustc --release --lib --features capi --crate-type staticlib

$(OUT)/bot_c: c/bot.c $(ROOT)/include/mcscon.h $(LIB)
	mkdir -p $(OUT) && $(CC) -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I$(ROOT)/include $< $(LDLIBS) -o $@

$(OUT)/bot_cpp: cpp/bot.cpp $(ROOT)/include/mcscon.hpp $(ROOT)/include/mcscon.h $(LIB)
	mkdir -p $(OUT) && $(CXX) -std=c++11 -Wall -Wextra -I$(ROOT)/include $< $(LDLIBS) -o $@

clean:
	rm -rf $(OUT)
.PHONY: all clean
