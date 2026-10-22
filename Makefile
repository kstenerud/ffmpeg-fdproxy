# Bare `make` lists the targets.
.DEFAULT_GOAL := help
OUT   ?= dist
BIN   ?= dist/bin-amd64
MEDIA ?= media
FUZZ_SECONDS ?= 600

.PHONY: help
help: ## List the targets
	@grep -E '^[a-z0-9-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "} {printf "  %-14s %s\n", $$1, $$2}'

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

.PHONY: fuzz
fuzz: ## Fuzz FFmpeg's side for FUZZ_SECONDS (default 600) under ASan and UBSan; needs make build-amd64's checkout
	fuzz/build.sh build/jellyfin-ffmpeg build/fuzz
	rm -rf build/fuzz/corpus && mkdir -p build/fuzz/corpus && cp fuzz/corpus/* build/fuzz/corpus/
	docker run --rm -v "$$PWD/build/fuzz":/work -w /work debian:trixie \
		./ffstreamer_fuzzer -max_len=61440 -max_total_time=$(FUZZ_SECONDS) -jobs=8 -workers=8 corpus
	@! ls build/fuzz/crash-* build/fuzz/leak-* build/fuzz/timeout-* build/fuzz/oom-* 2>/dev/null

.PHONY: clean
clean: ## Remove the jellyfin-ffmpeg checkout and the built tarballs
	rm -rf build dist
