# Terminator -- build with `make`; run with ./terminator  or  ./terminator useProcess=True
CC      ?= cc
UNAME   := $(shell uname -s)
WNOTRUNC := $(shell $(CC) -Wno-format-truncation -E -x c /dev/null >/dev/null 2>&1 && echo -Wno-format-truncation)
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter $(WNOTRUNC)

ifeq ($(UNAME),Darwin)
  # macOS: X11 comes from XQuartz (https://www.xquartz.org or `brew install --cask xquartz`)
  X11_CFLAGS := -I/opt/X11/include
  X11_LIBS   := -L/opt/X11/lib -lX11
else
  X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
  X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)
endif

# one source file per task (see README.md for the task -> file map)
SRCS = util.c task1_gui.c engine.c task2_commands.c task3_parser.c task3_expand.c \
       task4_5_pipes.c task6_multiwatch.c task7_hotkeys.c task7_editor.c task8_jobs.c \
       task9_history.c task10_completion.c task11_scripts.c
OBJS = $(SRCS:.c=.o)

terminator: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(X11_LIBS) -lpthread

%.o: %.c terminator.h
	$(CC) $(CFLAGS) $(X11_CFLAGS) -c $< -o $@

# headless tests: same sources, but main() of the GUI is renamed so tests/driver.c can drive the shell engines
test: $(SRCS) terminator.h tests/driver.c
	$(CC) -O1 -w $(X11_CFLAGS) -Dmain=terminator_main -o tests/driver $(SRCS) tests/driver.c $(X11_LIBS) -lpthread
	@cd tests && sh run_tests.sh

clean:
	rm -f terminator $(OBJS) tests/driver

.PHONY: test clean
