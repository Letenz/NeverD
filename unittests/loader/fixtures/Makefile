# Rebuild recipe for the COFF symbol table fixtures.
#
# mingw_sections.exe is linked without the CRT so that its symbol table holds
# only what one object contributes: GNU ld's section symbols (.text, .data,
# .bss, ...), the object's .file record, and its functions and variables.

MINGW_CC ?= x86_64-w64-mingw32-gcc
MINGW32_CC ?= i686-w64-mingw32-gcc
CFLAGS := -O1 -nostdlib -Wl,-e,entry -Wl,--no-insert-timestamp
# i386 C names carry a leading underscore.
CFLAGS32 := -O1 -nostdlib -Wl,-e,_entry -Wl,--no-insert-timestamp

.PHONY: all clean

all: mingw_sections.exe mingw_text_data.exe mingw_text_data32.exe \
     mingw_imp_slots.exe

mingw_sections.exe: mingw_sections.c
	$(MINGW_CC) $(CFLAGS) $< -o $@

# mingw_text_data.exe keeps a pointer table among its code, as GNU ld keeps
# __CTOR_LIST__ in .text, and refers to it from .data.
mingw_text_data.exe: mingw_text_data.c
	$(MINGW_CC) $(CFLAGS) $< -o $@

# mingw_text_data32.exe is its i386 build, whose pointers are four bytes.
mingw_text_data32.exe: mingw_text_data.c
	$(MINGW32_CC) $(CFLAGS32) $< -o $@

# mingw_imp_slots.exe calls functions through `__imp_X` variables, as MinGW's
# runtime defines them, one of them bound lazily.
mingw_imp_slots.exe: mingw_imp_slots.c
	$(MINGW_CC) $(CFLAGS) $< -o $@

clean:
	rm -f mingw_sections.exe mingw_text_data.exe mingw_text_data32.exe \
	  mingw_imp_slots.exe
