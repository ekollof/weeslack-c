# weeslack - Slack plugin for WeeChat
# Requires: weechat-dev, json-c, openssl

PLUGIN_NAME = weeslack
PLUGIN_FILE = $(PLUGIN_NAME).so

CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pedantic -O2
CFLAGS += -fPIC $(shell pkg-config --cflags weechat json-c openssl)
LDFLAGS += $(shell pkg-config --libs weechat json-c openssl)

# Install to system dir if root, otherwise user weechat plugin dir
ifeq ($(shell id -u),0)
  PREFIX ?= /usr/local
  LIBDIR ?= $(PREFIX)/lib/weechat
else
  LIBDIR ?= $(HOME)/.local/share/weechat/plugins
endif

SRCS = $(PLUGIN_NAME).c slack_http.c slack_ws.c slack_data.c slack_buffer.c slack_event.c
OBJS = $(SRCS:.c=.o)

all: $(PLUGIN_FILE)

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

.PHONY: all clean install uninstall
