#pragma once

#if defined(BO2ZM_GUEST_MEMORY_PLAIN)


typedef struct {
  unsigned char bytes[0x100001000ull];
} bo2zm_guest_all;
#define REX_GUEST_BARRIER() __asm__ __volatile__("" : "+m"(*(bo2zm_guest_all*)base))

typedef u8 __attribute__((may_alias)) bo2zm_guest_u8;
typedef u16 __attribute__((may_alias)) bo2zm_guest_u16;
typedef u32 __attribute__((may_alias)) bo2zm_guest_u32;
typedef u64 __attribute__((may_alias)) bo2zm_guest_u64;

#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(bo2zm_guest_u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(bo2zm_guest_u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(bo2zm_guest_u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(bo2zm_guest_u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U8(x, y) (*(bo2zm_guest_u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(bo2zm_guest_u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(bo2zm_guest_u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(bo2zm_guest_u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))

#else

#define REX_GUEST_BARRIER() ((void)0)

#endif
