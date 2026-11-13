// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Xiaomi Redmi Pad SE (xun, SM6225) eMMC 5.1 physical geometry:
//   - Sector size: 512 bytes (versus 4096 bytes on UFS)
//   - Userdata GPT boundaries (LBA): 20,129,584 (0x01332730) .. 244,318,207 (0x0E8FFFFF)
constexpr uint32_t kEmmcSectorBytes = 512U;
constexpr uint64_t kXunUserdataDefaultFirstLba = 0x01332730ULL;
constexpr uint64_t kXunUserdataDefaultLastLba  = 0x0E8FFFFFULL;

constexpr uint64_t kGptHeaderMagic   = 0x5452415020494645ULL;  // "EFI PART"
constexpr uint64_t kShngRootMagic    = 0x544F4F52474E4853ULL;  // "SHNGROOT"
constexpr uint64_t kShngPolicyMagic  = 0x314C4F50474E4853ULL;  // "SHNGPOL1"
constexpr size_t   kShngBlockBytes   = 4096U;
constexpr size_t   kMaxRootWindows   = 16U;

struct __attribute__((packed)) GptHeader {
  uint64_t signature;
  uint32_t revision;
  uint32_t headerSize;
  uint32_t headerCrc32;
  uint32_t reserved;
  uint64_t currentLba;
  uint64_t backupLba;
  uint64_t firstUsableLba;
  uint64_t lastUsableLba;
  uint8_t  diskGuid[16];
  uint64_t entriesLba;
  uint32_t entriesCount;
  uint32_t entrySize;
  uint32_t entriesCrc32;
};

struct __attribute__((packed)) GptEntry {
  uint8_t  typeGuid[16];
  uint8_t  partGuid[16];
  uint64_t firstLba;
  uint64_t lastLba;
  uint64_t attributes;
  uint16_t nameUtf16[36];
};

struct __attribute__((packed)) ShngRootEntry {
  char     name[32];
  uint64_t offset;
  uint64_t size;
  uint32_t blockSize;
  uint32_t flags;
  uint8_t  sha256[32];
  uint8_t  reserved[40];
};
static_assert(sizeof(ShngRootEntry) == 128U, "SHNGROOT entry must be 128 bytes");

struct __attribute__((packed)) ShngRootHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t count;
  uint64_t imageBytes;
  ShngRootEntry entries[16];
  uint8_t  padding[4064U - 24U - 16U * 128U];
  uint8_t  sha256[32];
};
static_assert(sizeof(ShngRootHeader) == 4096U, "SHNGROOT header must be 4096 bytes");

struct __attribute__((packed)) ShngPolicyEntry {
  char     name[32];
  uint64_t offset;
  uint64_t size;
  uint32_t blockSize;
  uint32_t flags;
  uint8_t  sha256[32];
};
static_assert(sizeof(ShngPolicyEntry) == 88U, "SHNGPOL1 entry must be 88 bytes");

struct __attribute__((packed)) ShngPolicyHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t count;
  uint32_t entryBytes;
  uint32_t reserved;
  uint8_t  entries[4064U - 24U];
  uint8_t  sha256[32];
};
static_assert(sizeof(ShngPolicyHeader) == 4096U, "SHNGPOL1 header must be 4096 bytes");

struct RootWindowDesc {
  bool     present;
  char     name[32];
  uint64_t userdataOffset;
  uint64_t byteSize;
  uint64_t startLba;
  uint64_t sectorCount;
  uint32_t blockSize;
  uint32_t flags;
  uint8_t  sha256[32];
  bool     hasPolicy;
  uint64_t policyUserdataOffset;
  uint64_t policyByteSize;
  uint8_t  policySha256[32];
};

struct EmmcRootLayout {
  bool     gptFound;
  bool     userdataFound;
  bool     manifestValid;
  bool     policiesPresent;
  uint32_t failStep;
  uint64_t userdataFirstLba;
  uint64_t userdataLastLba;
  uint64_t userdataSectors;
  uint64_t imageBytes;
  uint32_t windowCount;
  RootWindowDesc os;
  RootWindowDesc systemOs;
  RootWindowDesc data;
};

namespace xun_root_detail {

inline uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc = (crc >> 1) ^ mask;
    }
  }
  return ~crc;
}

inline bool isUserdataUtf16Name(const uint16_t name[36]) {
  static constexpr char target[] = "userdata";
  for (size_t i = 0; i < 8U; ++i) {
    uint16_t c = name[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<uint16_t>(c + ('a' - 'A'));
    if (c != static_cast<uint16_t>(target[i])) return false;
  }
  return name[8] == 0U;
}

inline void copyName(char dst[32], const char src[32]) {
  size_t i = 0;
  for (; i < 31U && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

inline bool nameEquals(const char *a, const char *b) {
  return strncmp(a, b, 32U) == 0;
}

}  // namespace xun_root_detail

// Parses eMMC GPT -> "userdata" -> SHNGROOT (+ optional SHNGPOL1).
// readSectors(ctx, lba, count, dst) reads `count` 512-byte sectors.
// sha256(data, len, digestOut) hashes [0..4064) for SHNGROOT/SHNGPOL1.
template <typename ReadFn, typename ShaFn>
inline bool parseEmmcRootLayout(ReadFn readSectors, ShaFn sha256,
                                uint8_t *scratch16K, size_t scratchSize,
                                EmmcRootLayout *out) {
  memset(out, 0, sizeof(*out));
  if (!scratch16K || scratchSize < 16384U) {
    out->failStep = 1;
    return false;
  }

  // 1. Read GPT header at LBA 1 (512 bytes)
  if (!readSectors(1ULL, 1U, scratch16K)) {
    out->failStep = 2;
    return false;
  }
  GptHeader gpt = {};
  memcpy(&gpt, scratch16K, sizeof(gpt));
  if (gpt.signature != kGptHeaderMagic || gpt.headerSize < 92U || gpt.headerSize > 512U ||
      gpt.entrySize != 128U || gpt.entriesCount == 0U || gpt.entriesCount > 256U) {
    out->failStep = 3;
    return false;
  }
  // Verify GPT Header CRC32 with headerCrc32 field zeroed
  uint8_t hdrCopy[512] = {};
  memcpy(hdrCopy, scratch16K, gpt.headerSize);
  memset(hdrCopy + 16U, 0, 4U);
  if (xun_root_detail::crc32(hdrCopy, gpt.headerSize) != gpt.headerCrc32) {
    out->failStep = 4;
    return false;
  }
  out->gptFound = true;

  // 2. Scan GPT entries in chunks <= 32 sectors (16 KiB) to locate "userdata"
  const uint32_t entriesTotalBytes = gpt.entriesCount * gpt.entrySize;
  const uint32_t entriesSectors = (entriesTotalBytes + kEmmcSectorBytes - 1U) / kEmmcSectorBytes;
  uint32_t entriesCrc = 0xFFFFFFFFU;
  uint32_t remainingEntriesBytes = entriesTotalBytes;

  uint32_t lbaOffset = 0U;
  while (lbaOffset < entriesSectors) {
    uint32_t chunkSectors = entriesSectors - lbaOffset;
    if (chunkSectors > 32U) chunkSectors = 32U;
    if (!readSectors(gpt.entriesLba + lbaOffset, chunkSectors, scratch16K)) {
      out->failStep = 5;
      return false;
    }
    const uint32_t chunkBytes = chunkSectors * kEmmcSectorBytes;
    const uint32_t crcBytes = (remainingEntriesBytes < chunkBytes) ? remainingEntriesBytes : chunkBytes;
    for (uint32_t b = 0; b < crcBytes; ++b) {
      entriesCrc ^= scratch16K[b];
      for (int bit = 0; bit < 8; ++bit) {
        const uint32_t mask = (entriesCrc & 1U) ? 0xEDB88320U : 0U;
        entriesCrc = (entriesCrc >> 1) ^ mask;
      }
    }
    remainingEntriesBytes -= crcBytes;

    const uint32_t countInChunk = chunkBytes / 128U;
    for (uint32_t e = 0; e < countInChunk; ++e) {
      GptEntry entry = {};
      memcpy(&entry, scratch16K + e * 128U, sizeof(entry));
      if (entry.firstLba != 0ULL && entry.lastLba >= entry.firstLba &&
          xun_root_detail::isUserdataUtf16Name(entry.nameUtf16)) {
        out->userdataFound = true;
        out->userdataFirstLba = entry.firstLba;
        out->userdataLastLba = entry.lastLba;
        out->userdataSectors = entry.lastLba - entry.firstLba + 1ULL;
      }
    }
    lbaOffset += chunkSectors;
  }
  if ((~entriesCrc) != gpt.entriesCrc32 || !out->userdataFound || out->userdataSectors < 2048ULL) {
    out->failStep = 6;
    return false;
  }

  // 3. Read SHNGROOT header (8 sectors = 4096 bytes) at userdataFirstLba
  if (!readSectors(out->userdataFirstLba, 8U, scratch16K)) {
    out->failStep = 7;
    return false;
  }
  ShngRootHeader root = {};
  memcpy(&root, scratch16K, sizeof(root));
  if (root.magic != kShngRootMagic || root.version != 1U ||
      root.count == 0U || root.count > kMaxRootWindows) {
    out->failStep = 8;
    return false;
  }
  uint8_t digest[32] = {};
  sha256(&root, 4064U, digest);
  if (memcmp(digest, root.sha256, 32U) != 0) {
    out->failStep = 9;
    return false;
  }

  out->manifestValid = true;
  out->imageBytes = root.imageBytes;
  out->windowCount = root.count;
  const uint64_t userdataTotalBytes = out->userdataSectors * kEmmcSectorBytes;

  for (uint32_t i = 0; i < root.count; ++i) {
    const ShngRootEntry &src = root.entries[i];
    if (src.offset == 0ULL || src.size == 0ULL ||
        (src.offset % kEmmcSectorBytes) != 0ULL ||
        (src.size % kEmmcSectorBytes) != 0ULL ||
        src.offset > userdataTotalBytes || src.size > userdataTotalBytes - src.offset) {
      out->failStep = 10;
      return false;
    }
    RootWindowDesc parsed = {};
    parsed.present = true;
    xun_root_detail::copyName(parsed.name, src.name);
    parsed.userdataOffset = src.offset;
    parsed.byteSize = src.size;
    parsed.startLba = out->userdataFirstLba + (src.offset / kEmmcSectorBytes);
    parsed.sectorCount = src.size / kEmmcSectorBytes;
    parsed.blockSize = src.blockSize ? src.blockSize : kEmmcSectorBytes;
    parsed.flags = src.flags;
    memcpy(parsed.sha256, src.sha256, 32U);

    if (xun_root_detail::nameEquals(parsed.name, "OS")) {
      out->os = parsed;
    } else if (xun_root_detail::nameEquals(parsed.name, "SystemOS")) {
      out->systemOs = parsed;
    } else if (xun_root_detail::nameEquals(parsed.name, "Data")) {
      out->data = parsed;
    }
  }

  if (!out->os.present || !out->systemOs.present) {
    out->failStep = 11;
    return false;
  }

  // 4. Read optional SHNGPOL1 locator block at userdataFirstLba + 8 (offset 4096).
  // Note: Initial userdata images shipped without SHNGPOL1 leave this block zeroed.
  if (readSectors(out->userdataFirstLba + 8ULL, 8U, scratch16K)) {
    ShngPolicyHeader pol = {};
    memcpy(&pol, scratch16K, sizeof(pol));
    if (pol.magic == kShngPolicyMagic && pol.version == 1U &&
        pol.entryBytes == sizeof(ShngPolicyEntry) && pol.count <= 40U) {
      sha256(&pol, 4064U, digest);
      if (memcmp(digest, pol.sha256, 32U) == 0) {
        out->policiesPresent = true;
        for (uint32_t i = 0; i < pol.count; ++i) {
          ShngPolicyEntry pe = {};
          memcpy(&pe, pol.entries + i * sizeof(ShngPolicyEntry), sizeof(pe));
          auto attach = [&](RootWindowDesc &w) {
            if (w.present && xun_root_detail::nameEquals(w.name, pe.name) &&
                pe.size >= 544ULL && pe.offset <= userdataTotalBytes &&
                pe.size <= userdataTotalBytes - pe.offset) {
              w.hasPolicy = true;
              w.policyUserdataOffset = pe.offset;
              w.policyByteSize = pe.size;
              memcpy(w.policySha256, pe.sha256, 32U);
            }
          };
          attach(out->os);
          attach(out->systemOs);
        }
      }
    }
  }

  return true;
}
