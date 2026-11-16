CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c
HEADERS = src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h src/temps_frame.h src/hub_frames.h
.PHONY: assets test

assets:
	$(PYTHON) tools/make_bars.py
	$(PYTHON) tools/make_drive_apps.py
	$(PYTHON) tools/make_boostdeck.py

build:
	mkdir -p build

build/boostdeck-collector: src/collector.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/shift-deck: src/collector.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -DSHIFT_DECK_ONLY $(filter %.c,$^) -lm -o $@

build/mhd-maps: src/collector.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -DMHD_MAPS_ONLY $(filter %.c,$^) -lm -o $@

build/diag-hub: src/collector.c src/diag_hub.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -DDIAG_HUB $(filter %collector.c,$^) $(CORE) -lm -o $@

build/test-apps: tests/test_apps.c src/fuel_blend.h src/perf_timer.h src/engine_temps.h src/lockup.h src/hub_frames.h | build
	$(CC) $(CFLAGS) -Isrc tests/test_apps.c -lm -o $@

build/test-core: tests/test_core.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

test: build/boostdeck-collector build/shift-deck build/mhd-maps build/test-core build/diag-hub build/test-apps
	./build/test-core
	./build/test-apps
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_drive_apps.py
	$(PYTHON) -B tests/test_diag_hub.py
