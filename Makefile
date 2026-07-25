# Makefile
include config.mk

# Compilation Targets
all: engine helpers

engine: src/engine/main.c src/engine/js_bindings.c src/engine/ui_render.c
	$(CC) $(CFLAGS) -Iinclude -Ilibs/lvgl -Ilibs/quickjs $^ -o bin/browser_engine $(LDFLAGS) -lquickjs -llvgl

helpers: webp_worker parser_worker

webp_worker: src/helpers/worker_webp.c
	$(MUSL_CC) -static -O3 -Iinclude $^ -o bin/static_webp_worker -lwebp

parser_worker: src/helpers/html_parser.c
	$(MUSL_CC) -static -O2 -Iinclude $^ -o bin/static_html_parser

clean:
	rm -rf bin/*
	