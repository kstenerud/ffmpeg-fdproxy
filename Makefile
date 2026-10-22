# Bare `make` lists the targets.
.DEFAULT_GOAL := help
OUT   ?= dist
BIN   ?= dist/bin-amd64
MEDIA ?= media

.PHONY: help
help: ## List the targets
	@grep -E '^[a-z0-9-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "} {printf "  %-14s %s\n", $$1, $$2}'

.PHONY: build-amd64
build-amd64: ## Build the patched FFmpeg for linux/amd64 into OUT (default dist/); needs Docker
	./build.sh amd64 $(OUT)

.PHONY: build-arm64
build-arm64: ## Build the patched FFmpeg for linux/arm64 into OUT (default dist/); needs Docker
	./build.sh arm64 $(OUT)

.PHONY: test
test: ## Run the suite against BIN (default dist/bin-amd64) with MEDIA (film.mkv, film-moov-at-end.mp4)
	python3 test/suite.py --bin $(BIN) --media $(MEDIA)

.PHONY: clean
clean: ## Remove the jellyfin-ffmpeg checkout and the built tarballs
	rm -rf build dist
