#ifndef CRC32_DEFS_H
#define CRC32_DEFS_H

#include<stdint.h>

/*
 * There are multiple 16-bit CRC polynomials in common use, but this is
 * *the* standard CRC-32 polynomial, first popularized by Ethernet.
 * x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x^1+x^0
 */
#define CRC32_POLY_LE 0xedb88320
#define CRC32_POLY_BE 0x04c11db7

/*
 * This is the CRC32c polynomial, as outlined by Castagnoli.
 * x^32+x^28+x^27+x^26+x^25+x^23+x^22+x^20+x^19+x^18+x^14+x^13+x^11+x^10+x^9+
 * x^8+x^6+x^0
 */
#define CRC32C_POLY_LE 0x82F63B78
#define CRC32C_POLY_BE 0x1EDC6F41

/* How many bits at a time to use.  Valid values are 1, 2, 4, 8, 32 and 64. */
/* For less performance-sensitive, use 4 */
#ifndef CRC_LE_BITS
# define CRC_LE_BITS 64
#endif
#ifndef CRC_BE_BITS
# define CRC_BE_BITS 64
#endif

/*
 * Little-endian CRC computation.  Used with serial bit streams sent
 * lsbit-first.  Be sure to use cpu_to_le32() to append the computed CRC.
 */
#if CRC_LE_BITS > 64 || CRC_LE_BITS < 1 || CRC_LE_BITS == 16 || \
	CRC_LE_BITS & CRC_LE_BITS-1
# error "CRC_LE_BITS must be one of {1, 2, 4, 8, 32, 64}"
#endif

/*
 * Big-endian CRC computation.  Used with serial bit streams sent
 * msbit-first.  Be sure to use cpu_to_be32() to append the computed CRC.
 */
#if CRC_BE_BITS > 64 || CRC_BE_BITS < 1 || CRC_BE_BITS == 16 || \
	CRC_BE_BITS & CRC_BE_BITS-1
# error "CRC_BE_BITS must be one of {1, 2, 4, 8, 32, 64}"
#endif


#define ___constant_swab32(x) \
	((uint32_t)( \
		(((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) | \
		(((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) | \
		(((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) | \
		(((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24)))


#if (__GNUC__ >= 3)
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)
#else
#define likely(x)	(x)
#define unlikely(x)	(x)
#endif

// Return a masked representation of crc.
//
// Motivation: it is problematic to compute the CRC of a string that
// contains embedded CRCs.  Therefore we recommend that CRCs stored
// somewhere (e.g., in files) should be masked before being stored.
uint32_t Mask(uint32_t crc);

// Return the crc whose masked representation is masked_crc.
uint32_t Unmask(uint32_t masked_crc); 

uint32_t DecodeFixed32(const char* ptr); 

void EncodeFixed32(char* buf, uint32_t value); 

/*
 * Interface functions
 */
uint32_t crc32_le(uint32_t crc, unsigned char const *p, size_t len);
uint32_t crc32_be(uint32_t crc, unsigned char const *p, size_t len);

#define crc32(seed, data, length)  crc32_le(seed, (unsigned char const *)(data), length)


#endif		// CRC32_DEFS_H
