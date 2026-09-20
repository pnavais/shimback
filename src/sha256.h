#ifndef SHIMBACK_SHA256_H
#define SHIMBACK_SHA256_H

#include <stdbool.h>

/* Computes the SHA-256 of the file at `path` and writes it to `out_hex` as
 * 64 lowercase hex digits plus a terminating NUL. Returns false if the file
 * can't be read. Native, so `update` can verify a download without depending
 * on sha256sum/shasum being installed. */
bool sha256_file(const char *path, char out_hex[65]);

#endif /* SHIMBACK_SHA256_H */
