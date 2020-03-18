export CFLAGS := -Wall -Wextra

.PHONY: ipc

all: ipc

ipc:
	$(MAKE) -C ipc

