SHELL := /bin/bash

.PHONY: check test generate clean icon cleanup

check:
	./scripts/local_check.sh --with-tests

test:
	swift test --package-path Packages/PocketDSHKit

generate:
	xcodegen generate --spec project.yml

icon:
	python3 scripts/generate_icon.py

clean:
	rm -rf build artifacts PocketDSH.xcodeproj PocketDSH/Resources/Info.plist

cleanup:
	./scripts/cleanup_local.sh --days 14
