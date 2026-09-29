obj-m += zimacube_bay.o

NAME    := zimacube-bay
VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
SRCDIR  := /usr/src/$(NAME)-$(VERSION)

# DKMS sets KERNELRELEASE when it rebuilds for a kernel other than the running
# one; without this the build would target uname -r instead.
KVER ?= $(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# plain module install: breaks on the next kernel upgrade, prefer `make dkms`
install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
	@echo
	@echo "Now: modprobe zimacube_bay (on a matching ZimaCube Pro)"

# --- dkms -----------------------------------------------------------------

check-version:
	@test -n "$(VERSION)" || { echo "could not read PACKAGE_VERSION from dkms.conf" >&2; exit 1; }

# install or update; safe to re-run after editing the source
dkms: check-version
	install -D -m 0644 zimacube_bay.c $(SRCDIR)/zimacube_bay.c
	install -D -m 0644 Makefile           $(SRCDIR)/Makefile
	install -D -m 0644 dkms.conf          $(SRCDIR)/dkms.conf
	@dkms status -m $(NAME) -v $(VERSION) | grep -q . || dkms add -m $(NAME) -v $(VERSION)
	# build --force is required: install --force alone reinstalls the cached
	# object and silently skips compiling an edited source
	dkms build   -m $(NAME) -v $(VERSION) --force
	dkms install -m $(NAME) -v $(VERSION) --force
	@echo
	@echo "Then: modprobe zimacube_bay (on a matching ZimaCube Pro)"

dkms-remove: check-version
	@dkms remove -m $(NAME) -v $(VERSION) --all 2>/dev/null || true
	rm -rf $(SRCDIR)

# drop every registered version; this is the one to use after a version bump,
# because dkms-remove reads the version from dkms.conf and would remove the new
# one rather than the old
dkms-purge:
	@for v in $$(dkms status -m $(NAME) | sed -n 's/^$(NAME)[/,] *\([^,:]*\).*/\1/p' | sort -u); do \
		echo "removing $(NAME)/$$v"; \
		dkms remove -m $(NAME) -v $$v --all 2>/dev/null || true; \
	done
	rm -rf /usr/src/$(NAME)-*

.PHONY: all clean install check-version dkms dkms-remove dkms-purge
