.PHONY: all clean core monitor policy

all: core monitor policy

core:
	$(MAKE) -C core

monitor: core
	$(MAKE) -C monitor

policy: core monitor
	$(MAKE) -C policy

clean:
	$(MAKE) -C core clean
	$(MAKE) -C monitor clean
	$(MAKE) -C policy clean
	find . -type f \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' -o -name '*.symvers' -o -name '*.order' -o -name '.*.cmd' \) -delete
	find . -type d -name '.tmp_versions' -exec rm -rf {} + 2>/dev/null || true