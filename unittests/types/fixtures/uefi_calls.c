// A UEFI application calling the firmware through the system table it is
// given, which NeverD's uefi-* libraries declare as EDK II's MdePkg does:
// its entry takes the specification's EFI_IMAGE_ENTRY_POINT, so its
// accesses read the system table's and the boot services' members.  Built
// with clang and lld-link against MdePkg/Include (I) of edk2-stable202608:
//   clang --target=x86_64-unknown-windows -ffreestanding -fshort-wchar \
//     -mno-red-zone -nostdinc -isystem $(clang -print-resource-dir)/include \
//     -I$I -I$I/X64 -O1 -fno-stack-protector -c uefi_calls.c -o uefi.obj
//   lld-link /subsystem:efi_application /entry:EfiMain /nodefaultlib \
//     /out:uefi_calls_x64.efi uefi.obj
// and uefi_calls_x86.efi with --target=i686-unknown-windows, -I$I/Ia32 and
// /machine:x86, uefi_calls_arm64.efi with --target=aarch64-unknown-windows,
// -I$I/AArch64 and /machine:arm64.
#include <Uefi.h>

// A protocol's header needs <Uefi.h>'s types first.
#include <Protocol/LoadedImage.h>

EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle,
                          EFI_SYSTEM_TABLE *SystemTable) {
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  SystemTable->ConOut->OutputString(SystemTable->ConOut, L"NeverD\r\n");
  EFI_STATUS Status = SystemTable->BootServices->HandleProtocol(
      ImageHandle, &LoadedImageGuid, (VOID **)&Image);
  if (EFI_ERROR(Status))
    return Status;
  return Image->ImageSize > 0x1000 ? EFI_SUCCESS : EFI_LOAD_ERROR;
}
