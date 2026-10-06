# Regenerates the checked-in unpacking fixtures. Every other suite builds what
# it tests; these images come from a packer, which the build cannot assume, so
# the originals are checked in beside the packed files made from them.
#
#   make PACKER=/path/to/upx
#
# The checked-in files were produced by Clang 23.1.2 with lld-link, GCC 13
# (x86_64-w64-mingw32) and UPX 5.2.1. Inputs carry no timestamps, so the same
# tools reproduce the same bytes; SHA256SUMS records them. The packer is not
# named UPX here because upx reads its options from that environment variable.
CLANG ?= clang
LLD_LINK ?= lld-link
MINGW_CC ?= x86_64-w64-mingw32-gcc
PACKER ?= upx
OUT := binaries
WORK := $(OUT)/.work

METHODS := nrv2b nrv2d nrv2e lzma
PACKED := $(foreach M,$(METHODS),$(OUT)/plain.upx-$(M).exe) $(OUT)/crt.upx.exe

all: $(OUT)/plain.exe $(OUT)/crt.exe $(PACKED) $(OUT)/SHA256SUMS
	rm -rf $(WORK)

$(WORK):
	mkdir -p $@

$(WORK)/kernel32.lib: | $(WORK)
	printf 'LIBRARY kernel32.dll\nEXPORTS\n  GetStdHandle\n  WriteFile\n  ExitProcess\n' > $(WORK)/kernel32.def
	$(LLD_LINK) /lib /machine:x64 /def:$(WORK)/kernel32.def /out:$@

$(OUT)/plain.exe: plain.c $(WORK)/kernel32.lib
	$(CLANG) --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding \
	  -fno-builtin -fno-stack-protector -fno-vectorize -fno-slp-vectorize \
	  -O1 -c plain.c -o $(WORK)/plain.obj
	$(LLD_LINK) /nodefaultlib /entry:entry /subsystem:console /machine:x64 \
	  /base:0x140000000 /timestamp:0 $(WORK)/plain.obj \
	  $(WORK)/kernel32.lib /out:$@

$(OUT)/crt.exe: crt.c
	$(MINGW_CC) -O2 -s -Wl,--no-insert-timestamp crt.c -o $@

$(OUT)/plain.upx-%.exe: $(OUT)/plain.exe
	cp $< $@
	$(PACKER) -q --$* $@

$(OUT)/crt.upx.exe: $(OUT)/crt.exe
	cp $< $@
	$(PACKER) -q $@

$(OUT)/SHA256SUMS: $(OUT)/plain.exe $(OUT)/crt.exe $(PACKED)
	cd $(OUT) && sha256sum *.exe > SHA256SUMS

.PHONY: all
