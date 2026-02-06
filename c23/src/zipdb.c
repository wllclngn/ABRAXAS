/*
 * zipdb.c - ZIP code database lookup
 *
 * mmap'd binary search on us_zipcodes.bin.
 * Entry format: 5 bytes ASCII ZIP + 4 bytes float32 lat + 4 bytes float32 lon.
 * File header: 4 bytes uint32_t count (little-endian).
 */

#define _GNU_SOURCE

#include "zipdb.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static_assert(sizeof(float) == 4, "float must be 32 bits for zipdb format");

constexpr int ENTRY_SIZE = 13;  /* 5 + 4 + 4 */
constexpr int HEADER_SIZE = 4;  /* uint32_t count */

bool zipdb_lookup(const char *db_path, const char *zipcode,
                  float *lat, float *lon)
{
    if (!db_path || !zipcode || !lat || !lon) return false;

    /* Normalize to exactly 5 digits */
    char zip5[6];
    size_t zlen = strlen(zipcode);
    if (zlen > 5) zlen = 5;
    memset(zip5, '0', 5);
    memcpy(zip5 + (5 - zlen), zipcode, zlen);
    zip5[5] = '\0';

    int fd = open(db_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return false; }

    void *map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return false;

    const uint8_t *data = map;

    /* Read entry count */
    uint32_t count;
    memcpy(&count, data, sizeof(count));

    /* Binary search */
    bool found = false;
    int low = 0, high = (int)count - 1;

    while (low <= high) {
        int mid = low + (high - low) / 2;
        size_t offset = HEADER_SIZE + (size_t)mid * ENTRY_SIZE;

        int cmp = memcmp(data + offset, zip5, 5);
        if (cmp == 0) {
            memcpy(lat, data + offset + 5, sizeof(float));
            memcpy(lon, data + offset + 9, sizeof(float));
            found = true;
            break;
        } else if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    munmap(map, (size_t)st.st_size);
    return found;
}
