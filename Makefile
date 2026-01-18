# CTAE Root Makefile
# Builds all subsystem modules

# All modules: core, monitor, policy
SUBDIRS := core monitor policy

.PHONY: all clean install uninstall test help $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean 2>/dev/null || true; \
	done

install:
	$(MAKE) -C core install
	$(MAKE) -C monitor install
	$(MAKE) -C policy install

uninstall:
	$(MAKE) -C policy uninstall
	$(MAKE) -C monitor uninstall
	$(MAKE) -C core uninstall

test:
	$(MAKE) -C core test
	$(MAKE) -C monitor test
	$(MAKE) -C policy test

help:
	@echo "CTAE Build System - Complete"
	@echo "============================"
	@echo "Targets:"
	@echo "  all       - Build all modules (core, monitor, policy)"
	@echo "  clean     - Clean build artifacts"
	@echo "  install   - Load all modules into kernel"
	@echo "  uninstall - Remove all modules from kernel"
	@echo "  test      - Check module status and logs"
	@echo "  help      - Show this message"
	@echo ""
	@echo "Quick start:"
	@echo "  make && sudo make install && make test"