# weeslack - Slack plugin for WeeChat
# Requires: weechat-dev, json-c, openssl, libcurl
#
# Normal build:
#   make && make install
#
# AddressSanitizer (heap UAF / double-free / OOB):
#   make clean asan && make install
#   # WeeChat itself is not ASAN-built — preload libasan when starting:
#   ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
#     LD_PRELOAD="$(gcc -print-file-name=libasan.so)" weechat
#
# Or:  make asan-run   (prints the exact LD_PRELOAD command)

PLUGIN_NAME = weeslack
PLUGIN_FILE = $(PLUGIN_NAME).so

CC ?= gcc
PKG_CFLAGS := $(shell pkg-config --cflags weechat json-c openssl libcurl)
PKG_LIBS   := $(shell pkg-config --libs weechat json-c openssl libcurl)

# Base flags; -O2 for release, ASAN uses -O1 -g (set below).
CFLAGS_COMMON := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-Wall -Wextra -Werror -pedantic -fPIC $(PKG_CFLAGS)

ifeq ($(ASAN),1)
  # ASAN + light UBSAN; keep frame pointers for readable stacks.
  CFLAGS ?= $(CFLAGS_COMMON) -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined \
	-fno-sanitize-recover=address,undefined
  LDFLAGS += -fsanitize=address,undefined $(PKG_LIBS)
else
  CFLAGS ?= $(CFLAGS_COMMON) -O2
  LDFLAGS += $(PKG_LIBS)
endif

# Install to system dir if root, otherwise user weechat plugin dir
ifeq ($(shell id -u),0)
  PREFIX ?= /usr/local
  LIBDIR ?= $(PREFIX)/lib/weechat
else
  LIBDIR ?= $(HOME)/.local/share/weechat/plugins
endif

SRCS = $(PLUGIN_NAME).c slack_http.c slack_ws.c slack_data.c slack_buffer.c slack_event.c
OBJS = $(SRCS:.c=.o)

ASAN_LIB := $(shell $(CC) -print-file-name=libasan.so)

all: $(PLUGIN_FILE)

# Convenience: full clean rebuild with ASAN
asan:
	$(MAKE) clean
	$(MAKE) ASAN=1 all

asan-install: asan
	$(MAKE) ASAN=1 install
	@echo ""
	@echo "Installed ASAN weeslack.so → $(LIBDIR)/"
	@echo "Start WeeChat with libasan preloaded (host binary is not ASAN-linked):"
	@echo "  ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \\"
	@echo "    LD_PRELOAD=\"$(ASAN_LIB)\" weechat"
	@echo ""
	@echo "Prefer process restart over /plugin reload while hunting crashes."

asan-run:
	@echo "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \\"
	@echo "  LD_PRELOAD=\"$(ASAN_LIB)\" weechat"

$(PLUGIN_FILE): $(OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

%.o: %.c $(PLUGIN_NAME).h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(PLUGIN_FILE)

install: $(PLUGIN_FILE)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(PLUGIN_FILE) $(DESTDIR)$(LIBDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(PLUGIN_FILE)

.PHONY: all clean install uninstall asan asan-install asan-run
