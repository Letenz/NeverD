# Rebuild recipe for the COFF symbol table fixture.
#
# mingw_sections.exe is linked without the CRT so that its symbol table holds
# only what one object contributes: GNU ld's section symbols (.text, .data,
# .bss, ...), the object's .file record, and its functions and variables.

MINGW_CC ?= x86_64-w64-mingw32-gcc
CFLAGS := -O1 -nostdlib -Wl,-e,entry -Wl,--no-insert-timestamp

.PHONY: all clean

all: mingw_sections.exe

mingw_sections.exe: mingw_sections.c
	$(MINGW_CC) $(CFLAGS) $< -o $@

clean:
	rm -f mingw_sections.exe
