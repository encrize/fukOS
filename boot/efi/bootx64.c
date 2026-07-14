

#include "efi.h"

#define KERNEL_PHYS   0x00100000ULL
#define KERNEL_PAGES  512
#define KERNEL_MAX    (KERNEL_PAGES * 4096ULL)
#define PART_SECTORS  262144ULL
#define SECTOR        512ULL

#define KERNEL_TOP    0x04000000ULL

typedef struct {
	UINT32 fb_addr;
	UINT32 pitch;
	UINT32 width;
	UINT32 height;
	UINT32 bpp;
	UINT32 storage_addr;
	UINT32 storage_size;
} boot_params;

extern void tramp_to_kernel(boot_params *bp);

static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;

static void print(CHAR16 *s) { gST->ConOut->OutputString(gST->ConOut, s); }

static void print_hex(UINT64 v) {
	CHAR16 buf[19];
	const CHAR16 *hex = u"0123456789ABCDEF";
	int i;
	buf[0] = u'0'; buf[1] = u'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
	buf[18] = 0;
	print(buf);
}

static void die(CHAR16 *msg) {
	print(u"\r\n[MyOS UEFI] FATAL: ");
	print(msg);
	print(u"\r\nHalting.\r\n");
	for (;;) __asm__ __volatile__("hlt");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS st;
	gST = SystemTable;
	gBS = SystemTable->BootServices;

	print(u"MyOS UEFI loader\r\n");

	static boot_params bp;

	EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
	st = gBS->LocateProtocol(&gop_guid, 0, (void **)&gop);
	if (EFI_ERROR(st) || !gop) die(u"no Graphics Output Protocol");

	bp.fb_addr = (UINT32)gop->Mode->FrameBufferBase;
	bp.width   = gop->Mode->Info->HorizontalResolution;
	bp.height  = gop->Mode->Info->VerticalResolution;
	bp.bpp     = 32;
	bp.pitch   = gop->Mode->Info->PixelsPerScanLine * 4;
	if (gop->Mode->Info->PixelFormat == EFI_PIXEL_FORMAT_RGBX8)
		print(u"warning: framebuffer is RGB; colours may be swapped\r\n");

	print(u"  framebuffer "); print_hex(gop->Mode->FrameBufferBase);
	print(u" "); print_hex(bp.width); print(u"x"); print_hex(bp.height);
	print(u"\r\n");

	EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *li = 0;
	st = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
	if (EFI_ERROR(st) || !li) die(u"no LoadedImage protocol");

	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
	st = gBS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
	if (EFI_ERROR(st) || !fs) die(u"boot volume has no FAT filesystem");

	EFI_FILE_PROTOCOL *root = 0, *kf = 0;
	st = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(st)) die(u"OpenVolume failed");
	st = root->Open(root, &kf, u"KERNEL.BIN", EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st))
		st = root->Open(root, &kf, u"\\EFI\\BOOT\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st) || !kf) die(u"KERNEL.BIN not found on the volume");

	EFI_PHYSICAL_ADDRESS kaddr = KERNEL_PHYS;
	st = gBS->AllocatePages(AllocateAddress, EfiLoaderData, KERNEL_PAGES, &kaddr);
	if (EFI_ERROR(st)) die(u"cannot reserve 0x100000 for the kernel");

	{
		UINT8 *dst = (UINT8 *)(UINTN)KERNEL_PHYS;
		UINTN  total = 0;
		for (;;) {
			UINTN n = 0x10000;
			if (total + n > KERNEL_MAX) n = (UINTN)(KERNEL_MAX - total);
			if (n == 0) die(u"kernel larger than reserved region");
			st = kf->Read(kf, &n, dst + total);
			if (EFI_ERROR(st)) die(u"error reading KERNEL.BIN");
			if (n == 0) break;
			total += n;
		}
		kf->Close(kf);
		print(u"  kernel bytes "); print_hex(total); print(u"\r\n");
	}

	EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
	EFI_BLOCK_IO_PROTOCOL *bio = 0;
	st = gBS->HandleProtocol(li->DeviceHandle, &bio_guid, (void **)&bio);
	if (!EFI_ERROR(st) && bio && bio->Media && bio->Media->MediaPresent) {
		UINT32 bs = bio->Media->BlockSize ? bio->Media->BlockSize : 512;
		UINT64 nblocks = PART_SECTORS;

		if (bio->Media->LastBlock + 1 < nblocks) nblocks = bio->Media->LastBlock + 1;
		UINT64 bytes = nblocks * bs;
		UINTN  pages = (UINTN)((bytes + 4095) / 4096);

		static const EFI_PHYSICAL_ADDRESS cand[] = {
			0x04000000ULL, 0x08000000ULL, 0x10000000ULL, 0x40000000ULL
		};
		EFI_PHYSICAL_ADDRESS saddr = 0;
		int got = 0;
		for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
			EFI_PHYSICAL_ADDRESS a = cand[i];
			st = gBS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &a);
			if (!EFI_ERROR(st)) { saddr = a; got = 1; break; }
		}

		if (!got) {
			EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFFULL;
			st = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &a);
			if (!EFI_ERROR(st) && a >= KERNEL_TOP) { saddr = a; got = 1; }
		}
		if (got) {
			st = bio->ReadBlocks(bio, bio->Media->MediaId, 0, (UINTN)bytes,
			                     (void *)(UINTN)saddr);
			if (!EFI_ERROR(st)) {
				bp.storage_addr = (UINT32)saddr;
				bp.storage_size = (UINT32)bytes;
				print(u"  storage "); print_hex(saddr);
				print(u" size "); print_hex(bytes); print(u"\r\n");
			} else {
				print(u"  warning: ReadBlocks failed, no storage\r\n");
			}
		} else {
			print(u"  warning: could not reserve storage RAM, no storage\r\n");
		}
	} else {
		print(u"  warning: no Block I/O on boot volume, no storage\r\n");
	}

	void  *mmap = 0;
	UINTN  msize = 0, mkey = 0, dsize = 0;
	UINT32 dver = 0;

	gBS->GetMemoryMap(&msize, 0, &mkey, &dsize, &dver);
	msize += 8 * (dsize ? dsize : 48);
	st = gBS->AllocatePool(EfiLoaderData, msize, (void **)&mmap);
	if (EFI_ERROR(st)) die(u"AllocatePool for memory map failed");

	int ok = 0;
	for (int tries = 0; tries < 8; tries++) {
		UINTN cur = msize;
		st = gBS->GetMemoryMap(&cur, mmap, &mkey, &dsize, &dver);
		if (EFI_ERROR(st)) die(u"GetMemoryMap failed");
		st = gBS->ExitBootServices(ImageHandle, mkey);
		if (!EFI_ERROR(st)) { ok = 1; break; }

	}
	if (!ok) die(u"ExitBootServices failed");

	tramp_to_kernel(&bp);
	return EFI_SUCCESS;
}
