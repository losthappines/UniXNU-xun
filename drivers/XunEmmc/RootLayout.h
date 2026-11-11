// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production parser for Redmi Pad SE (xun, SM6225) eMMC 5.1 storage.
// Sector size: 512 bytes (NOT 4096-byte UFS sectors).
// Controller: Qualcomm SDHCI MSM at 0x04744000.
// Userdata start LBA: 20129584 (0x1332730), last LBA: 244318207 (0xe8fffff).

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define EMMC_SECTOR_SIZE        512U
#define EMMC_SECTORS_PER_4K     8U
#define XUN_USERDATA_FIRST_LBA  0x01332730ULL
#define XUN_USERDATA_LAST_LBA   0x0E8FFFFFULL

struct RootWindow {
  char name[32];
  uint64_t offset;   // bytes from the start of the userdata partition
  uint64_t size;     // bytes
  uint8_t sha256[32];
};

struct RootPolicy {
  char name[32];
  uint64_t offset;   // bytes from the start of the userdata partition
  uint64_t size;     // bytes
  uint8_t sha256[32];
};

struct RootLayout {
  uint64_t userdataFirst, userdataLast;   // 512-byte LBAs on eMMC
  uint64_t imageBytes;                    // size of flashed root image from manifest
  uint64_t gptEntriesLba;
  uint32_t gptHeaderCrc, gptEntriesCrc, gptEntryCount;
  uint32_t windows;
  RootWindow window[4];
  uint32_t manifestStatus;                // 0 ok, 1 no manifest, 2 bad magic/count, 3 hash, 4 bounds, 5 names, 6 overlap
  uint32_t policyStatus;                  // 0 ok, 1 no policy, 2 bad magic/version/count, 3 hash, 4 bounds, 5 names, 6 overlap
  uint32_t policies;
  RootPolicy policy[4];
  uint32_t magic0, magic1;                // first 8 bytes of partition's first block
  uint32_t ext4Magic;
  uint32_t ext4Blocks;
};

#ifndef kDataMax
#define kDataMax 16384
#endif

static uint8_t gScratch[kDataMax];

static inline uint32_t le32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}

static inline uint64_t le64 (const uint8_t *at)
{
  return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
}

static inline uint32_t rootCrc32 (const uint8_t *data, size_t bytes, bool header = false)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}

static inline bool rootEntryUsed (const uint8_t *entry)
{
  uint8_t used = 0;
  for (unsigned i = 0; i < 16; ++i) used |= entry[i];
  return used != 0;
}

// Find the partition named "userdata" in eMMC primary GPT (512-byte sectors).
// LBA 1: GPT header (512 bytes).
// LBA 2..33: GPT partition entries (32 sectors = 16,384 bytes, 128 entries x 128 bytes).
template<typename ReadFn>
static bool findEmmcRootLayout (RootLayout *layout, ReadFn hostReadSectors, const char **why)
{
  memset (layout, 0, sizeof (*layout));

  // 1. Read GPT Header at LBA 1 (1 sector = 512 bytes)
  if (hostReadSectors (1, 1, gScratch) != 0) {
    *why = "GPT header read failed";
    return false;
  }
  if (le32 (gScratch) != 0x20494645U || le32 (gScratch + 4) != 0x54524150U || le32 (gScratch + 84) != 128) {
    *why = "GPT header signature invalid";
    return false;
  }
  uint32_t headerBytes = le32 (gScratch + 12);
  if (le32 (gScratch + 8) != 0x00010000 || headerBytes < 92 || headerBytes > 512 ||
      le32 (gScratch + 20) || le64 (gScratch + 24) != 1 ||
      rootCrc32 (gScratch, headerBytes, true) != le32 (gScratch + 16)) {
    *why = "GPT header CRC/revision mismatch";
    return false;
  }

  uint64_t backup = le64 (gScratch + 32), first = le64 (gScratch + 40), last = le64 (gScratch + 48);
  uint32_t entriesCrc = le32 (gScratch + 88);
  uint64_t entriesLba = le64 (gScratch + 72);
  uint32_t count = le32 (gScratch + 80);
  layout->gptHeaderCrc = le32 (gScratch + 16);
  layout->gptEntriesCrc = entriesCrc;
  layout->gptEntriesLba = entriesLba;
  layout->gptEntryCount = count;

  if (entriesLba < 2 || count > 128) {
    *why = "GPT entry array geometry invalid";
    return false;
  }

  // 2. Read partition entries (128 entries * 128 bytes = 16384 bytes = 32 sectors of 512 bytes)
  uint32_t entrySectors = (count * 128 + EMMC_SECTOR_SIZE - 1) / EMMC_SECTOR_SIZE;
  if (hostReadSectors (entriesLba, entrySectors, gScratch) != 0) {
    *why = "GPT entries read failed";
    return false;
  }
  if (rootCrc32 (gScratch, count * 128) != entriesCrc) {
    *why = "GPT entries CRC mismatch";
    return false;
  }

  // 3. Locate "userdata" partition
  static const char wanted[] = "userdata";
  bool found = false;
  for (uint32_t index = 0; index < count; index++) {
    const uint8_t *entry = gScratch + index * 128;
    if (!rootEntryUsed (entry)) continue;
    uint64_t entryFirst = le64 (entry + 32), entryLast = le64 (entry + 40);
    if (entryFirst < first || entryLast < entryFirst || entryLast > last) {
      *why = "GPT partition bounds out of range";
      return false;
    }
    bool match = true;
    for (unsigned c = 0; c < sizeof (wanted) - 1; c++) {
      if (entry[56 + 2 * c] != static_cast<uint8_t> (wanted[c]) || entry[57 + 2 * c] != 0) {
        match = false;
        break;
      }
    }
    if (match) {
      if (found) {
        *why = "duplicate userdata partition";
        return false;
      }
      layout->userdataFirst = entryFirst;
      layout->userdataLast = entryLast;
      found = true;
    }
  }

  if (!found || layout->userdataLast < layout->userdataFirst) {
    *why = "no userdata partition found in GPT";
    return false;
  }

  // Verify against xun verified geometry
  if (layout->userdataFirst != XUN_USERDATA_FIRST_LBA) {
    *why = "userdata first LBA does not match xun geometry (expected 0x1332730)";
    return false;
  }

  return true;
}
