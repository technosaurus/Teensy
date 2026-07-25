# Makefile
include config.mk

# Compilation Targets
all: teensy helpers

teensy: src/main.c src/js_bindings.c src/ui_render.c
	$(CC) $(CFLAGS) -Iinclude -Ilibs/lvgl -Ilibs/quickjs $^ -o bin/teensy $(LDFLAGS) -lquickjs -llvgl

helpers: splat parse get

splat: src/splat.c
	$(MUSL_CC) -static -Os -Iinclude $^ -o bin/splat -lwebp

parse: src/parse.c
	$(MUSL_CC) -static -Os -Iinclude $^ -o bin/parse

get: src/get.c
	$(MUSL_CC) -static -Os -Iinclude $^ -o bin/get

clean:
	rm -rf bin/*
	
