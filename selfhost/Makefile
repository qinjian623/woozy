PYTHON ?= python3
STAGES ?= 3
OPT ?= 0
JOBS ?= 1

# Capture MAKEFILE_LIST at this entry, after any preloaded makefiles. Pass it
# and the literal option values as data, not shell code.
override export _BOOTSTRAP_MAKEFILE := $(MAKEFILE_LIST)
ifneq ($(origin OUT),undefined)
override export OUT := $(value OUT)
endif
override export STAGE0 := $(value STAGE0)
override export STRACE := $(value STRACE)
override export RUNTIME_DIR := $(value RUNTIME_DIR)
override export TIMEOUT := $(value TIMEOUT)
override export STAGES := $(value STAGES)
override export OPT := $(value OPT)
override export JOBS := $(value JOBS)

# Try the longest suffix first so spaces in the entry filename stay intact.
.PHONY: bootstrap
bootstrap:
	@entry=$$_BOOTSTRAP_MAKEFILE; \
	while :; do \
	    dir=.; case "$$entry" in */*) dir=$${entry%/*} ;; esac; \
	    if [ -f "$$entry" ] && [ -f "$$dir/bootstrap.py" ]; then break; fi; \
	    case "$$entry" in \
	        *' '*) entry=$${entry#* } ;; \
	        *) printf '%s\n' 'Cannot locate the selfhost Makefile and bootstrap.py' >&2; exit 1 ;; \
	    esac; \
	done; \
	out=$$OUT; if [ "$${OUT+x}" != x ]; then \
	    parent=$$(CDPATH= cd -- "$$dir/../.." && pwd) || exit; out=$$parent/woozy-bootstrap; \
	fi; \
	set -- --source-make --out-dir "$$out" --stages "$$STAGES" --opt "$$OPT" --jobs "$$JOBS"; \
	if [ -n "$$STAGE0" ]; then set -- "$$@" --stage0 "$$STAGE0"; fi; \
	if [ -n "$$STRACE" ]; then set -- "$$@" --strace "$$STRACE"; fi; \
	if [ -n "$$RUNTIME_DIR" ]; then set -- "$$@" --runtime-dir "$$RUNTIME_DIR"; fi; \
	if [ -n "$$TIMEOUT" ]; then set -- "$$@" --timeout "$$TIMEOUT"; fi; \
	$(PYTHON) -B "$$dir/bootstrap.py" "$$@"
