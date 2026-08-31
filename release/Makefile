# Zeitlos release tooling.
#
# A thin wrapper over ./zrelease, for people (and muscle memory) that
# expect `make`. Everything here is one line; the real thing is the
# Python. `./zrelease --help` lists the options this does not expose.
#
#   make -C release list
#   make -C release check
#   make -C release build VERSION=v0.0.3
#   make -C release verify VERSION=v0.0.3
#   make -C release ship VERSION=v0.0.3
#   make -C release clean
#
# Restrict a build to some targets:
#
#   make -C release build VERSION=v0.0.3 TARGETS="lakritz_uart mozart_ml1"

Z = ./zrelease

ifdef TARGETS
TARGET_ARG = --targets $(TARGETS)
endif

# Passed through verbatim, for the --allow-* and --bump switches:
#   make -C release build VERSION=v0.0.3 ARGS="--bump --allow-timing-fail"
ARGS ?=

.PHONY: list check layout build verify ship clean help

help:
	@sed -n '3,20p' Makefile | sed 's/^# \?//'

list:
	@$(Z) list

check:
	@$(Z) check

layout:
	@$(Z) layout

build: require-version
	@$(Z) build $(VERSION) $(TARGET_ARG) $(ARGS)

verify: require-version
	@$(Z) verify $(VERSION)

ship: require-version
	@$(Z) ship $(VERSION) $(ARGS)

clean:
	@$(Z) clean

require-version:
ifndef VERSION
	@echo "set VERSION, e.g. make -C release build VERSION=v0.0.3"
	@exit 1
endif
