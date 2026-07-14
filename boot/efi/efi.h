

#ifndef MYOS_EFI_H
#define MYOS_EFI_H

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed   long long  INT64;
typedef UINT64              UINTN;
typedef INT64               INTN;
typedef unsigned short      CHAR16;
typedef unsigned char       BOOLEAN;
typedef void               *EFI_HANDLE;
typedef UINTN               EFI_STATUS;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef UINT64              EFI_LBA;
typedef UINTN               EFI_TPL;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS            0u
#define EFI_BUFFER_TOO_SMALL   5u

#define EFI_ERROR(s)  (((INTN)(EFI_STATUS)(s)) < 0)

#define AllocateAnyPages    0
#define AllocateMaxAddress  1
#define AllocateAddress     2

#define EfiLoaderCode  1
#define EfiLoaderData  2

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	void       *Reset;
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
	                                  CHAR16 *String);

};

typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *RaiseTPL;
	void *RestoreTPL;

	EFI_STATUS (EFIAPI *AllocatePages)(UINTN Type, UINTN MemoryType,
	                                   UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
	void *FreePages;
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap,
	                                  UINTN *MapKey, UINTN *DescriptorSize,
	                                  UINT32 *DescriptorVersion);
	EFI_STATUS (EFIAPI *AllocatePool)(UINTN PoolType, UINTN Size, void **Buffer);
	EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
	                                    void **Interface);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;

	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

	void *GetNextMonotonicCount;
	EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
	void *SetWatchdogTimer;

	void *ConnectController;
	void *DisconnectController;

	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;

	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration,
	                                    void **Interface);

} EFI_BOOT_SERVICES;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16      *FirmwareVendor;
	UINT32       FirmwareRevision;
	EFI_HANDLE   ConsoleInHandle;
	void        *ConIn;
	EFI_HANDLE   ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE   StandardErrorHandle;
	void        *StdErr;
	void        *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN        NumberOfTableEntries;
	void        *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
	UINT32       Revision;
	EFI_HANDLE   ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE   DeviceHandle;
	void        *FilePath;
	void        *Reserved;
	UINT32       LoadOptionsSize;
	void        *LoadOptions;
	void        *ImageBase;
	UINT64       ImageSize;
	UINTN        ImageCodeType;
	UINTN        ImageDataType;
	void        *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct _EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New,
	                          CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
	EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
	void *Delete;
	EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
	void *Write;
	void *GetPosition;
	EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Position);
	void *GetInfo;
	void *SetInfo;
	void *Flush;
};
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
	                                EFI_FILE_PROTOCOL **Root);
};

typedef struct {
	UINT32  MediaId;
	BOOLEAN RemovableMedia;
	BOOLEAN MediaPresent;
	BOOLEAN LogicalPartition;
	BOOLEAN ReadOnly;
	BOOLEAN WriteCaching;
	UINT32  BlockSize;
	UINT32  IoAlign;
	EFI_LBA LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
struct _EFI_BLOCK_IO_PROTOCOL {
	UINT64 Revision;
	EFI_BLOCK_IO_MEDIA *Media;
	void *Reset;
	EFI_STATUS (EFIAPI *ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
	                                EFI_LBA LBA, UINTN BufferSize, void *Buffer);
	void *WriteBlocks;
	void *FlushBlocks;
};

typedef struct {
	UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;
typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	UINT32 PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN  SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
	void *QueryMode;
	void *SetMode;
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

#define EFI_PIXEL_FORMAT_RGBX8  0
#define EFI_PIXEL_FORMAT_BGRX8  1

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define EFI_BLOCK_IO_PROTOCOL_GUID \
	{0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}

#endif
