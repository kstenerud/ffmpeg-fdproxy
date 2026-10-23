# Bare `make` lists the targets.
.DEFAULT_GOAL := help
OUT   ?= dist
BIN   ?= dist/bin-amd64
MEDIA ?= media
FUZZ_SECONDS ?= 600
FUZZ_JOBS ?= 8

.PHONY: help
help: ## List the targets
	@grep -E '^[a-z0-9-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "} {printf "  %-14s %s\n", $$1, $$2}'

.PHONY: check-patch
check-patch: ## Check that patches/9000 adds src/libavformat/ffstreamer.c as it stands
	python3 tools/check_patch.py

.PHONY: check-spec
check-spec: ## Check that ffstreamer-specification.md embeds ffstreamer.dogma as it stands
	python3 tools/check_spec.py

.PHONY: patch
patch: ## Rewrite patches/9000's ffstreamer.c from src/libavformat/ffstreamer.c, where it is edited
	python3 tools/sync_patch.py

.PHONY: checkout
checkout: ## Check out jellyfin-ffmpeg at JELLYFIN_TAG into build/, with patches/ after its series
	./checkout.sh

.PHONY: build-amd64
build-amd64: ## Build the patched FFmpeg for linux/amd64 into OUT (default dist/); needs Docker
	./build.sh amd64 $(OUT)

.PHONY: build-arm64
build-arm64: ## Build the patched FFmpeg for linux/arm64 into OUT (default dist/); needs Docker
	./build.sh arm64 $(OUT)

.PHONY: media
media: ## Make the suite's films into MEDIA (default media/) from FFmpeg's test sources, with BIN's ffmpeg
	python3 test/make_media.py --ffmpeg $(BIN)/ffmpeg $(MEDIA)

.PHONY: test
test: ## Run the suite against BIN (default dist/bin-amd64) with MEDIA (film.mkv, film-moov-at-end.mp4)
	python3 test/suite.py --bin $(BIN) --media $(MEDIA)

.PHONY: test-claims
test-claims: checkout ## Check that a pipe serves one session at a time and a closed one's descriptor serves the next
	fuzz/build.sh build/jellyfin-ffmpeg build/fuzz
	docker run --rm -v "$$PWD/build/fuzz":/work -w /work debian:trixie ./claims_test

.PHONY: upstream
upstream: ## Make the series for FFmpeg's own tree, at UPSTREAM_COMMIT, into build/upstream/
	tools/upstream.sh build/upstream

.PHONY: test-upstream
test-upstream: ## Make the series, then build it and run its FATE test and the protocols manual's build
	tools/upstream.sh --test build/upstream

.PHONY: fuzz
fuzz: checkout ## Fuzz FFmpeg's side for FUZZ_SECONDS (default 600) on FUZZ_JOBS workers under ASan and UBSan
	fuzz/build.sh build/jellyfin-ffmpeg build/fuzz
	rm -rf build/fuzz/corpus && mkdir -p build/fuzz/corpus && cp fuzz/corpus/* build/fuzz/corpus/
	docker run --rm -v "$$PWD/build/fuzz":/work -w /work debian:trixie \
		./ffstreamer_fuzzer -max_len=61440 -max_total_time=$(FUZZ_SECONDS) -jobs=$(FUZZ_JOBS) -workers=$(FUZZ_JOBS) corpus
	@! ls build/fuzz/crash-* build/fuzz/leak-* build/fuzz/timeout-* build/fuzz/oom-* 2>/dev/null

.PHONY: clean
clean: ## Remove the jellyfin-ffmpeg checkout and the built tarballs
	rm -rf build dist
