# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude -Ithird_party/bzip2 $(VERFLAGS)

# The version this binary says it is: the release in include/pkg.h, a patch
# number raised by hand when a release changes, and the day it was built.
# PATCH=n BUILD=20260920 on the command line pins them for a reproducible build.
PATCH    ?= 1
BUILD    ?= $(shell date -u +%Y%m%d)
BUILDDAY ?= $(shell date -u +%d.%m.%Y)
VERFLAGS  = -DPKG_VERSION_PATCH='"$(PATCH)"' -DPKG_BUILD='"$(BUILD)"' -DPKG_BUILD_DAY='"$(BUILDDAY)"' 

# The sources, listed once for every build: see sources.mk.
include sources.mk
HDR  = $(wildcard include/*.h) $(wildcard src/*.h)

UNITS = test_args test_activity test_container test_sha256 test_manifest test_ed25519 test_image test_ameta test_target test_application

.PHONY: all print-sources check-symbols check-cross test test-run test-ubsan check-portability check-m68k check-image check check-aros clean install aros-channel

all: build/apkg build/libapkg.a

build/apkg-provisional: src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPKG_FUSION_CLI -o $@ src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST)

# The product name and the development alias are byte-identical common clients.
build/apkg: build/apkg-core
	cp build/apkg-core $@

.PHONY: test-fusion test-contract
test-contract:
	python3 tests/contracts/check.py

test-fusion: test-contract test-apkg build/apkg build/apkg-provisional build/pkg build/test_target build/test_application build/test_first_install build/test_arospkg_read
	python3 tests/test_http_fixture.py
	./build/test_target
	./build/test_application
	./build/test_first_install ./build/pkg
	./build/test_arospkg_read
	python3 tests/provisional-cli.py
	python3 tests/fusion-cli.py
	python3 tests/fusion-publish.py

# Compile the imported AROS core with a host-only DOS adapter; no test double
# is linked into either product. This checks the real pkg_open implementation.
AROSPKG_READ_TEST_SOURCES = tests/test_arospkg_read.c components/arospkg/src/libpkg/core.c components/arospkg/src/libpkg/json.c components/arospkg/src/libpkg/err.c components/arospkg/src/libpkg/sha256.c
AROSPKG_READ_TEST_HEADERS = $(wildcard components/arospkg/src/libpkg/*.h tests/context_stubs/proto/*.h tests/context_stubs/exec/*.h tests/context_stubs/dos/*.h)
build/test_arospkg_read: $(AROSPKG_READ_TEST_SOURCES) $(AROSPKG_READ_TEST_HEADERS)
	@mkdir -p build
	$(CC) -D_DARWIN_C_SOURCE -std=c99 -Wall -Wextra -Itests/context_stubs -Icomponents/arospkg/src/libpkg -o $@ $(AROSPKG_READ_TEST_SOURCES)

build/test_first_install: tests/test_first_install.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(CORE) $(HOST)

# The list, for a build that is not this Makefile (the AROS scripts, the
# tests that compile pkg themselves): `make -s print-sources`.
print-sources:
	@echo src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# Every check this tree knows how to run.
check: check-portability check-symbols check-cross test test-ubsan check-m68k check-image

.PHONY: check-legacy-symbols
check-symbols: check-common-symbols check-legacy-symbols

# A program links libpkg.a next to its own code and libc, so a global of the
# library named warn or hint would take the program's calls: libc has warn.
# Every global the library defines is named pkg_, pkgi_ (shared between its
# own modules, see src/pkg_internal.h), PKG_ or BZ2_, and bz_internal_error,
# which bzip2 asks the program to define. Verified to be able to fail: a
# function made global without its prefix is named here and exits 1.
check-legacy-symbols: build/libpkg.a
	@want=$$(for o in $(LIBOBJ); do basename $$o; done | sort); \
	have=$$(ar t build/libpkg.a | grep '\.o$$' | sort); \
	if [ "$$want" != "$$have" ]; then \
		echo "check-symbols: FAIL, libpkg.a holds other objects than sources.mk lists"; \
		printf '%s\n' "$$have" | grep -vxF "$$want" | sed 's/^/  not listed: /'; \
		printf '%s\n' "$$want" | grep -vxF "$$have" | sed 's/^/  missing:    /'; exit 1; fi
	@bad=$$(nm -g $(LIBOBJ) | awk 'NF==3 && $$2 != "U" {print $$3}' | sed 's/^_//' \
		| grep -vE '^(pkg_|pkgi_|PKG_|BZ2_|bz_internal_error$$)' | sort -u); \
	n=$$(nm -g $(LIBOBJ) | awk 'NF==3 && $$2 != "U"' | wc -l); \
	if [ "$$n" -eq 0 ]; then echo "check-symbols: FAIL, nm listed no symbol at all"; exit 1; fi; \
	if [ -n "$$bad" ]; then echo "check-symbols: FAIL, globals without the library's prefix:"; echo "$$bad"; exit 1; fi; \
	echo "check-symbols: PASS, $$n globals, every one prefixed"

# The builds the host compiler cannot see: each has failed on a function only
# its own #ifdef leaves unused, and -Werror makes that an error. Windows and
# both AROS CPUs, compiled every time, not at release. A missing toolchain
# fails here, naming it: a check that skips is a check that passes.
check-cross:
	@command -v $(WINCC) > /dev/null || { echo "check-cross: FAIL, $(WINCC) is not installed (brew install mingw-w64)"; exit 1; }
	@rm -f build/pkg.exe && $(MAKE) --no-print-directory build/pkg.exe > build/cross-windows.log 2>&1 \
		|| { cat build/cross-windows.log; echo "check-cross: FAIL, Windows"; exit 1; }
	@sh tools/build-aros.sh > build/cross-aros-aarch64.log 2>&1 \
		|| { tail -20 build/cross-aros-aarch64.log; echo "check-cross: FAIL, AROS aarch64 (see tools/build-aros.sh)"; exit 1; }
	@sh tools/build-aros-x86_64.sh > build/cross-aros-x86_64.log 2>&1 \
		|| { tail -20 build/cross-aros-x86_64.log; echo "check-cross: FAIL, AROS x86_64 (see tools/build-aros-x86_64.sh)"; exit 1; }
	@echo "check-cross: PASS, Windows x86_64, AROS aarch64 and AROS x86_64 build with -Werror"

build/pkg: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

# libpkg for other programs: a static library and pkg.h. The host layer is
# part of it, since every operation touches files.
# Exactly the listed objects, never whatever lies in build/lib-obj: after a
# source was split or renamed, its old object stayed there, went into the
# archive ahead of the new ones, and a program linked the old code.
LIBOBJ = $(patsubst %.c,build/lib-obj/%.o,$(notdir $(LIB) $(CORE) $(HOST)))

build/libpkg.a: $(LIB) $(CORE) $(HOST) $(HDR)
	@rm -rf build/lib-obj && mkdir -p build/lib-obj
	@for f in $(LIB) $(CORE) $(HOST); do \
		$(CC) $(CFLAGS) $(CPPFLAGS) -c $$f -o build/lib-obj/$$(basename $$f .c).o || exit 1; \
	done
	@rm -f $@
	ar rcs $@ $(LIBOBJ)

# Install the development client and shared library. The inherited pkg skill
# describes the old interface and is not installed as guidance for apkg.
PREFIX ?= $(HOME)/.local

install: build/apkg build/libapkg.a
	@mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib"
	cp build/apkg "$(DESTDIR)$(PREFIX)/bin/apkg"
	cp build/apkg-core "$(DESTDIR)$(PREFIX)/bin/apkg-core"
	cp include/apkg.h include/apkg_client.h "$(DESTDIR)$(PREFIX)/include/"
	cp build/libapkg.a "$(DESTDIR)$(PREFIX)/lib/libapkg.a"
	@mkdir -p "$(DESTDIR)$(PREFIX)/libexec/apkg/tools" "$(DESTDIR)$(PREFIX)/libexec/apkg/build" "$(DESTDIR)$(PREFIX)/libexec/apkg/components/arospkg/tools" "$(DESTDIR)$(PREFIX)/share/doc/apkg-fusion"
	cp tools/apkg-build tools/apkg-channel tools/apkg-publish "$(DESTDIR)$(PREFIX)/libexec/apkg/tools/"
	chmod 755 "$(DESTDIR)$(PREFIX)/libexec/apkg/tools/apkg-build" "$(DESTDIR)$(PREFIX)/libexec/apkg/tools/apkg-channel" "$(DESTDIR)$(PREFIX)/libexec/apkg/tools/apkg-publish"
	cp components/arospkg/tools/apkg-pack components/arospkg/tools/mkindex.py components/arospkg/tools/catalogue.py components/arospkg/tools/publish_github.py "$(DESTDIR)$(PREFIX)/libexec/apkg/components/arospkg/tools/"
	cp components/arospkg/LICENSE "$(DESTDIR)$(PREFIX)/libexec/apkg/components/arospkg/LICENSE"
	cp LICENSE "$(DESTDIR)$(PREFIX)/share/doc/apkg-fusion/LICENSE"
	ln -sf ../../../bin/apkg "$(DESTDIR)$(PREFIX)/libexec/apkg/build/apkg"
	ln -sf ../libexec/apkg/tools/apkg-build "$(DESTDIR)$(PREFIX)/bin/apkg-build"
	ln -sf ../libexec/apkg/tools/apkg-channel "$(DESTDIR)$(PREFIX)/bin/apkg-channel"
	ln -sf ../libexec/apkg/tools/apkg-publish "$(DESTDIR)$(PREFIX)/bin/apkg-publish"
	@echo "installed common-engine apkg into $(DESTDIR)$(PREFIX)/bin"

# A channel that puts Pkg itself on AROS machines: `make aros-channel
# CHANNEL=<dir>`. See tools/make-aros-channel.sh.
aros-channel: build/pkg
	sh tools/make-aros-channel.sh "$(CHANNEL)"

# The examples, built against libpkg.a as another program would.
build/example-%: examples/%.c build/libpkg.a include/pkg.h include/pkg_activity.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

# The events example runs the operation on a thread of its own.
build/example-events: examples/events.c build/libpkg.a include/pkg.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ $< build/libpkg.a

# Windows, cross-built with mingw-w64. tools/make-windows-kit.sh wraps it in a
# test kit to run on a Windows machine.
WINCC ?= x86_64-w64-mingw32-gcc
WINHOST = src/pkg_fs_win32.c src/pkg_out.c src/pkg_port.c src/pkg_style.c

build/pkg.exe: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(WINHOST) $(HDR)
	@mkdir -p build
	$(WINCC) $(CFLAGS) $(CPPFLAGS) -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(WINHOST) \
		-lbcrypt -ladvapi32 -lshell32

build/apkg-provisional.exe: src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(WINHOST) $(HDR)
	@mkdir -p build
	$(WINCC) $(CFLAGS) $(CPPFLAGS) -DPKG_FUSION_CLI -o $@ src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(WINHOST) \
		-lbcrypt -ladvapi32 -lshell32

# macOS, one universal binary for Apple silicon and Intel.
build/pkg-macos: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	cc $(CFLAGS) $(CPPFLAGS) -arch arm64 -arch x86_64 -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

build/apkg-provisional-macos: src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	cc $(CFLAGS) $(CPPFLAGS) -DPKG_FUSION_CLI -arch arm64 -arch x86_64 -o $@ src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST)

# Linux, static against musl, cross-built with zig so no Linux toolchain is
# needed here.
build/pkg-linux-%: src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	zig cc -target $*-linux-musl $(CFLAGS) $(CPPFLAGS) -static -o $@ src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)

build/apkg-provisional-linux-%: src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	zig cc -target $*-linux-musl $(CFLAGS) $(CPPFLAGS) -DPKG_FUSION_CLI -static -o $@ src/pkg_main.c src/pkg_cli_fusion.c $(CLI) $(LIB) $(CORE) $(HOST)

# The library through pkg.h alone, as another program would use it.
build/test_api: tests/test_api.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_api.c $(LIB) $(CORE) $(HOST)

build/test_events: tests/test_events.c $(LIB) $(CORE) $(HOST) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_events.c $(LIB) $(CORE) $(HOST)

build/test_update: tests/test_update.c build/libpkg.a $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

build/test_update_config: tests/test_update_config.c src/pkg_update.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_update_config.c src/pkg_update.c

build/test_environment: tests/test_environment.c build/libpkg.a $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< build/libpkg.a

build/test_activity: tests/test_activity.c src/pkg_activity.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_activity.c src/pkg_activity.c

build/test_%: tests/test_%.c $(CORE) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CORE)

# `test` removes every binary first, deliberately. Editing a source and running
# the test inside the same second left a stale binary here once, and the test
# then reported the previous state of the code. A build this small buys nothing
# by being incremental, and a test that can report the wrong state is worse
# than a slow one.
test:
	python3 tests/atomic-aros.py
	@config=$$(mktemp -d); trap 'rm -rf "$$config"' EXIT HUP INT TERM; \
		XDG_CONFIG_HOME="$$config" $(MAKE) --no-print-directory test-run

test-run:
	@rm -f build/pkg build/test_api $(UNITS:%=build/%)
	@$(MAKE) --no-print-directory build/pkg build/test_api $(UNITS:%=build/%)
	@for t in $(UNITS) test_api; do echo "== $$t"; ./build/$$t || exit 1; done
	@echo "== e2e"
	@PKG=./build/pkg sh tests/e2e.sh
	@echo "== lock"
	@PKG=./build/pkg sh tests/lock.sh
	@echo "== key"
	@PKG=./build/pkg sh tests/key.sh
	@echo "== interrupt"
	@PKG=./build/pkg sh tests/interrupt.sh
	@echo "== deps"
	@PKG=./build/pkg sh tests/deps.sh
	@echo "== placement"
	@PKG=./build/pkg sh tests/placement.sh
	@echo "== crossarch"
	@PKG=./build/pkg sh tests/crossarch.sh
	@echo "== status"
	@PKG=./build/pkg sh tests/status.sh
	@echo "== activity"
	@PKG=./build/pkg sh tests/activity.sh
	@PKG=./build/pkg python3 tests/activity-network.py
	@echo "== events"
	@rm -f build/test_events
	@$(MAKE) --no-print-directory build/test_events
	@sh tests/events.sh
	@echo "== network"
	@PKG=./build/pkg sh tests/network.sh
	@echo "== channels"
	@PKG=./build/pkg sh tests/channels.sh
	@echo "== search"
	@PKG=./build/pkg sh tests/search.sh
	@echo "== push"
	@PKG=./build/pkg sh tests/push.sh
	@echo "== hostile"
	@PKG=./build/pkg sh tests/hostile.sh
	@echo "== resolve"
	@PKG=./build/pkg sh tests/resolve.sh
	@echo "== catalogue"
	@PKG=./build/pkg sh tests/catalogue.sh
	@echo "== pkginfo"
	@PKG=./build/pkg sh tests/pkginfo.sh
	@echo "== ssh-sign"
	@sh tests/ssh-sign.sh
	@echo "== docs-links"
	@sh tests/docs-links.sh
	@echo "== grammar"
	@PKG=./build/pkg python3 tests/grammar.py
	@echo "== docs-examples"
	@PKG=./build/pkg sh tests/docs-examples.sh
	@echo "== archive"
	@rm -f build/test_archive
	@$(MAKE) --no-print-directory build/test_archive
	@sh tests/archive.sh
	@echo "== fastarchive"
	@PKG=./build/pkg sh tests/fastarchive.sh
	@echo "== installer onboarding"
	@python3 tests/installer-onboarding.py
	@echo "== environments"
	@$(MAKE) --no-print-directory build/test_environment
	@./build/test_environment
	@PKG=./build/pkg python3 tests/environments_cli.py
	@echo "== selfupdate"
	@$(MAKE) --no-print-directory build/test_update build/test_update_config build/example-selfupdate
	@./build/test_update_config
	@PKG=./build/pkg python3 tests/selfupdate.py
	@PKG=./build/pkg python3 tests/selfupdate-bootstrap.py
	@echo "== examples"
	@rm -f build/libpkg.a build/example-basic build/example-browse build/example-events
	@$(MAKE) --no-print-directory build/example-basic build/example-browse build/example-events
	@PKG=./build/pkg sh tests/examples.sh

# The image writer judged by amitools, an FFS written apart from it. Needs the
# amitools virtualenv described at the top of tests/image.sh.
check-image: build/pkg
	@PKG=./build/pkg sh tests/image.sh

# Byte order is expressed in pkg_be32_get and pkg_be32_put and nowhere else.
# This refuses the constructs that would quietly reintroduce a host-order
# dependency. Verified to be able to fail: adding one of these to a source
# makes it report that file and exit non-zero.
#
# A socket address is the one thing outside the package format whose order
# the host's own network layer fixes, and htons is how it is written there.
# Such a line carries ALLOWED as a comment, so it says why it is there and
# every other use still fails.
FORBIDDEN = (hton[sl]|ntoh[sl]|__bswap|__builtin_bswap|BYTE_ORDER|BIG_END|LITTLE_END)
ALLOWED   = a socket port, not the package format

check-portability:
	@if grep -rnE '$(FORBIDDEN)' src include tests | grep -v '$(ALLOWED)'; then \
		echo "check-portability: FAIL, host byte order reached the sources"; \
		exit 1; \
	else \
		echo "check-portability: PASS, byte order lives only in the accessors"; \
	fi

# Unit tests and the host runs again, under the sanitizers. The
# misaligned-buffer test is the point for the container: a struct mapped over
# the stream would pass the plain build and fail here, the way it would fault
# on a 68000.
SAN = -O1 -g -fsanitize=undefined,address -fno-omit-frame-pointer

test-ubsan:
	@mkdir -p build/san
	@for t in $(UNITS); do \
		case $$t in \
			test_activity) with="src/pkg_activity.c" ;; \
			*)             with="$(CORE)" ;; \
		esac; \
		$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
			-o build/san/$$t tests/$$t.c $$with || exit 1; \
		./build/san/$$t > /dev/null || { echo "test-ubsan: $$t FAILED"; exit 1; }; \
	done
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/test_api tests/test_api.c $(LIB) $(CORE) $(HOST)
	@./build/san/test_api > /dev/null || { echo "test-ubsan: test_api FAILED"; exit 1; }
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/test_events tests/test_events.c $(LIB) $(CORE) $(HOST)
	@TEST_EVENTS=./build/san/test_events sh tests/events.sh > build/san/events.log 2>&1 \
		|| { tail -20 build/san/events.log; echo "test-ubsan: events FAILED"; exit 1; }
	@$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) $(CPPFLAGS) \
		-o build/san/pkg src/pkg_main.c $(CLI) $(LIB) $(CORE) $(HOST)
	@PKG=./build/san/pkg sh tests/e2e.sh > build/san/e2e.log 2>&1 \
		|| { tail -20 build/san/e2e.log; echo "test-ubsan: e2e FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/deps.sh > build/san/deps.log 2>&1 \
		|| { tail -20 build/san/deps.log; echo "test-ubsan: deps FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/status.sh > build/san/status.log 2>&1 \
		|| { tail -20 build/san/status.log; echo "test-ubsan: status FAILED"; exit 1; }
	@PKG=./build/san/pkg sh tests/image.sh > build/san/image.log 2>&1 \
		|| { tail -20 build/san/image.log; echo "test-ubsan: image FAILED"; exit 1; }
	@echo "test-ubsan: PASS, units, events, e2e, deps, status and image under -fsanitize=undefined,address"

# A big-endian COMPILE of the portable core. Running on a big-endian target is
# separate work and is not claimed here.
M68K_CC ?= m68k-aros-gcc

check-m68k:
	@if command -v "$(M68K_CC)" >/dev/null 2>&1; then \
		mkdir -p build/m68k; \
		for f in $(CORE); do \
			$(M68K_CC) -std=c99 -Wall -Wextra -Werror -O2 $(CPPFLAGS) \
				-c $$f -o build/m68k/$$(basename $$f .c).o || exit 1; \
		done; \
		echo "check-m68k: PASS, the portable core builds for a big-endian target"; \
	else \
		echo "check-m68k: SKIP, no m68k compiler at $(M68K_CC)"; \
	fi

# The hosted AROS check uses the build and SDK configured by tools/build-aros.sh
# and requires that no hosted instance is already running.
# Kept out of `check` because it boots an operating system.
check-aros: build/pkg
	@sh tools/build-aros.sh
	@sh tests/aros-smoke.sh

clean:
	rm -rf build

# Common context engine, separate from the provisional global-state backend.
APKG_ENGINE = src/apkg/inspect.c src/apkg/client_channel.c src/apkg/channel_install.c src/apkg/heads.c src/apkg/origin.c src/apkg/transfer.c src/apkg/http.c src/apkg/channel.c src/apkg/author.c src/apkg/source.c src/apkg/attributes.c src/apkg/blob.c src/apkg/archive.c src/apkg/client_ops.c src/apkg/bundle.c src/apkg/relations.c src/apkg/version.c src/apkg/tree.c src/apkg/operations.c src/apkg/alloc.c src/apkg/json.c src/apkg/context.c src/apkg/schema.c src/apkg/signed.c src/apkg/plan.c src/apkg/events.c src/apkg/execute.c src/apkg/client.c
APKG_SERVICES = src/pkg_ameta.c src/pkg_container.c src/pkg_sha256.c src/pkg_sha512.c src/pkg_ed25519.c src/pkg_target.c src/pkg_application.c src/pkg_fs_posix.c
build/libapkg-test.dylib: $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/pkg_ameta.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O1 -DPKG_FS_LOCAL_ONLY -DAPKG_TESTING -fPIC -shared -Iinclude -Isrc/apkg $(APKG_ENGINE) $(APKG_SERVICES) -o $@

.PHONY: test-apkg
test-apkg: build/test_preparation_memory build/test_channel_memory build/libapkg-test.dylib build/apkg build/apkg-core build/test_pm_common build/test_pm_rexx build/libapkg.a check-common-symbols
	./build/test_pm_rexx
	python3 -m unittest discover -s tests/apkg -v

PM_REXX_SOURCES = components/arospkg/tests/test_rexxcmd.c components/arospkg/src/pkgmanager/rexxcmd.c $(addprefix components/arospkg/src/libpkg/,updates.c entries.c app.c json.c listing.c req.c upgrade.c err.c)
build/test_pm_rexx: $(PM_REXX_SOURCES) components/arospkg/src/pkgmanager/rexxcmd.h $(wildcard components/arospkg/src/libpkg/*.h)
	@mkdir -p build
	$(CC) -O1 -Wall -Wextra -std=gnu99 -DAPKG_FUSION $(PM_REXX_SOURCES) -o $@
.PHONY: print-pm-rexx-sources
print-pm-rexx-sources:
	@echo $(PM_REXX_SOURCES)

build/apkg-core: src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/apkg_client.h include/pkg_ameta.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O2 -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

build/test_pm_common: tests/apkg/test_pm_worker.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/common_browser.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/apkg_client.h include/pkg_ameta.h components/arospkg/src/pkgmanager/job.h components/arospkg/src/pkgmanager/common_catalogue.h components/arospkg/src/pkgmanager/rexxcmd.h src/apkg/changes.inc src/apkg/internal.h
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O1 -DPKG_FS_LOCAL_ONLY -DAPKG_FUSION -Iinclude -Isrc/apkg -Itests/pm_stubs -Icomponents/arospkg/src/pkgmanager tests/apkg/test_pm_worker.c components/arospkg/src/pkgmanager/common_worker.c components/arospkg/src/pkgmanager/common_browser.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

build/test_channel_memory: tests/apkg/test_channel_memory.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/apkg_client.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O1 -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg tests/apkg/test_channel_memory.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

APKG_OBJECTS = $(patsubst %.c,build/common/%.o,$(APKG_ENGINE) $(APKG_SERVICES))
build/libapkg.a: $(APKG_OBJECTS)
	rm -f $@
	ar rcs $@ $(APKG_OBJECTS)

build/common/%.o: %.c include/apkg.h include/apkg_client.h include/pkg_fs.h include/pkg_ameta.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg -c $< -o $@

.PHONY: print-apkg-sources
print-apkg-sources:
	@echo $(APKG_ENGINE) $(APKG_SERVICES)

.PHONY: check-common-symbols
check-common-symbols: build/libapkg.a build/apkg-core build/apkg
	@bad=$$(nm -g build/libapkg.a | awk 'NF==3 && $$2 != "U" {print $$3}' | sed 's/^_//' | grep -vE '^(apkg_|ak_|pkg_)' | sort -u); 	if [ -n "$$bad" ]; then echo "unexpected common-library globals: $$bad"; exit 1; fi
	@if nm -g build/libapkg.a build/apkg-core build/apkg | grep -E 'apkg_test_|pkgi_' >/dev/null; then echo "test hooks or provisional request backend in common product"; exit 1; fi

build/test_preparation_memory: tests/apkg/test_preparation_memory.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	$(CC) -std=c99 -Wall -Wextra -Werror -O1 -DPKG_FS_LOCAL_ONLY -DAPKG_TESTING -Iinclude -Isrc/apkg tests/apkg/test_preparation_memory.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

# Product platform targets always select the common engine. Inherited variants
# retain explicit provisional names above; they are never a product fallback.
build/apkg-macos: src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/apkg_client.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	cc $(CFLAGS) -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg -arch arm64 -arch x86_64 src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

build/apkg-linux-%: src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) include/apkg.h include/apkg_client.h src/apkg/internal.h src/apkg/changes.inc
	@mkdir -p build
	zig cc -target $*-linux-musl $(CFLAGS) -DPKG_FS_LOCAL_ONLY -Iinclude -Isrc/apkg -static src/apkg/main.c $(APKG_ENGINE) $(APKG_SERVICES) -o $@

.PHONY: unsupported-common-windows
build/apkg.exe: unsupported-common-windows
unsupported-common-windows:
	@echo "Common Windows filesystem/process adapters are not implemented; build/pkg.exe is the explicit inherited baseline." >&2
	@exit 2
