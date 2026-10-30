# Makefile -- GCC/MinGW build. Use this when CMake is not installed.
#   mingw32-make            build ./mstutor.exe
#   mingw32-make test       build and run the self-tests
#   mingw32-make clean

CXX      ?= g++
CC       ?= gcc
# _WIN32_WINNT=0x0A00 exposes the Windows 10 per-monitor DPI entry points.
# -MMD -MP emit .d files so editing a header rebuilds everything that includes
# it. Without them a changed function signature links against a stale object
# and fails with a confusing "undefined reference".
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc -Ithird_party -DNOMINMAX \
            -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
            -DUNICODE -D_UNICODE -MMD -MP
CFLAGS   ?= -O2 -Ithird_party
SQLFLAGS  = -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
            -DSQLITE_OMIT_LOAD_EXTENSION

# -mwindows selects the GUI subsystem, so double-clicking the exe opens the
# window without a console flashing up. The console subcommands still work:
# main() attaches to the parent console when given one.
# Statically linked so the shipped exe needs no MinGW runtime DLLs.
# -municode makes the entry point wWinMain rather than WinMain, so the command
# line arrives as UTF-16 and non-ASCII paths survive.
LDFLAGS  = -static -static-libgcc -static-libstdc++ -mwindows -municode
LDLIBS   = -lbcrypt -lsecur32 -lcrypt32 -lws2_32 -lcomctl32 -lgdi32 -luser32 \
           -lshell32 -lole32 -ladvapi32

BUILD    = build
TARGET   = mstutor.exe

SOURCES = src/main.cpp src/util.cpp src/crypto.cpp src/config.cpp src/db.cpp \
          src/tls_socket.cpp src/mailer.cpp src/auth.cpp src/llm.cpp \
          src/guard.cpp src/rag.cpp src/chat.cpp src/markdown.cpp src/prefs.cpp src/credential_store.cpp \
          src/runtime.cpp src/gui.cpp src/gui_richtext.cpp src/gui_edit.cpp src/selftest.cpp

OBJECTS = $(patsubst src/%.cpp,$(BUILD)/%.o,$(SOURCES)) $(BUILD)/sqlite3.o \
          $(BUILD)/resources.o
DEPS    = $(patsubst src/%.cpp,$(BUILD)/%.d,$(SOURCES))

WINDRES ?= windres

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# The amalgamation is C, and takes a long time at -O2; it is built once.
$(BUILD)/sqlite3.o: third_party/sqlite3.c | $(BUILD)
	$(CC) $(CFLAGS) $(SQLFLAGS) -c -o $@ $<

# Application icon and version metadata. -I res so the .rc can find the .ico.
$(BUILD)/resources.o: res/mstutor.rc res/mstutor.ico | $(BUILD)
	$(WINDRES) -I res -i $< -O coff -o $@

$(BUILD):
	@mkdir $(BUILD) 2>nul || exit 0

test: $(TARGET)
	./$(TARGET) selftest

# $(RM) is make's own portable remove, which works whether make is driving
# cmd.exe or a Unix-style shell. "if exist ... rmdir" silently does nothing
# under the latter, which left stale objects behind and produced confusing
# link errors after a header change.
clean:
	-$(RM) $(BUILD)/*.o $(BUILD)/*.d $(TARGET)

# Generated header dependencies. The dash keeps the first build, when no .d
# files exist yet, from failing.
-include $(DEPS)
