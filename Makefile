# CTAE Root Makefile
# Builds all subsystem modules

# Phase 1: Only core is implemented
SUBDIRS := core monitor policy

.PHONY: all clean install uninstall test help $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $dir clean 2>/dev/null || true; \
	done

install:
	$(MAKE) -C core install

uninstall:
	$(MAKE) -C core uninstall

test:
	$(MAKE) -C core test

help:
	@echo "CTAE Build System - Phase 1"
	@echo "============================"
	@echo "Targets:"
	@echo "  all       - Build core module"
	@echo "  clean     - Clean build artifacts"
	@echo "  install   - Load core module into kernel"
	@echo "  uninstall - Remove core module from kernel"
	@echo "  test      - Check module status and logs"
	@echo "  help      - Show this message"
	@echo ""
	@echo "Quick start:"
	@echo "  make && sudo make install && make test"