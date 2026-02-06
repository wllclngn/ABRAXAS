/*
 * zipdb.h - ZIP code database lookup
 *
 * Binary search on mmap'd us_zipcodes.bin.
 * Format: u32 count, then count * 13-byte entries (5 ASCII + float32 lat + float32 lon).
 */

#ifndef ZIPDB_H
#define ZIPDB_H

#include <stdbool.h>

/* Look up coordinates for a 5-digit US ZIP code.
   Returns true on success, writing lat/lon. */
bool zipdb_lookup(const char *db_path, const char *zipcode,
                  float *lat, float *lon);

#endif /* ZIPDB_H */
