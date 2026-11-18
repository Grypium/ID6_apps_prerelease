CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c
HEADERS = src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h src/temps_frame.h src/hub_frames.h src/ethanol_source.h
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

# The hub carries upstream's MOST fallback; host builds use the fake DSI selected by ID6_FAKE_MOST.
MOST = src/most/most_link.c src/most/dsi_client.c src/most/most_proto.c src/most/dsi_bind.c
MOST_HEADERS = src/most/most_link.h src/most/dsi_client.h src/most/most_proto.h src/most/dsi_abi.h
FAKE_DSI = tests/fake_dsi.c tests/fake_dme.c tests/fake_most_env.c tests/fake_dsi.h tests/fake_dme.h

build/diag-hub: src/collector.c src/diag_hub.c $(CORE) $(MOST) $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS) | build
	$(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests -DDIAG_HUB src/collector.c $(CORE) $(MOST) $(filter %.c,$(FAKE_DSI)) -lm -o $@

build/test-most: tests/test_most.c $(MOST) $(FAKE_DSI) $(MOST_HEADERS) | build
	$(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests tests/test_most.c $(MOST) tests/fake_dsi.c tests/fake_dme.c -o $@

build/test-apps: tests/test_apps.c src/fuel_blend.h src/perf_timer.h src/engine_temps.h src/lockup.h src/hub_frames.h | build
	$(CC) $(CFLAGS) -Isrc tests/test_apps.c -lm -o $@

build/test-core: tests/test_core.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

test: build/boostdeck-collector build/shift-deck build/mhd-maps build/test-core build/diag-hub build/test-apps build/test-most
	./build/test-core
	./build/test-most
	./build/test-apps
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_drive_apps.py
	$(PYTHON) -B tests/test_diag_hub.py
