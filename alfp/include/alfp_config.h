/*
 * alfp_config.h - compile-time limits. Everything is statically allocated;
 * there is no malloc anywhere in the ALFP library.
 *
 * Override any value with -D on the compiler command line.
 */
#ifndef ALFP_CONFIG_H
#define ALFP_CONFIG_H

/* Largest file: ALFP_MAX_FRAGS fragments. At 236-241 bytes per fragment that is ~120 kB,
 * far above the 3-10 kB sample images, and keeps each bitmap at 64 bytes. */
#ifndef ALFP_MAX_FRAGS
#define ALFP_MAX_FRAGS 512u
#endif
#define ALFP_BITMAP_BYTES ((ALFP_MAX_FRAGS + 7u) / 8u)

/* Largest packet we ever build or accept (LoRa limit is 255 including the 5-byte mesh header). */
#ifndef ALFP_MAX_PKT
#define ALFP_MAX_PKT 250u
#endif

/* Opaque capture metadata carried in the manifest (sensor readings, labels). */
#ifndef ALFP_MAX_META
#define ALFP_MAX_META 64u
#endif

/* Concurrent incoming transfers at the bridge (one per field node is enough: nodes send one at a time). */
#ifndef ALFP_RX_SESSIONS
#define ALFP_RX_SESSIONS 4u
#endif

/* Recently completed sessions remembered so a lost ACK can be re-sent instead of restarting. */
#ifndef ALFP_DONE_MEMORY
#define ALFP_DONE_MEMORY 8u
#endif

#ifndef ALFP_PATH_MAX
#define ALFP_PATH_MAX 64u
#endif

/* Bytes of file hashed per alfp_step() while computing/verifying CRC-32.
 * Keeps each step short so the radio is serviced every few ms. */
#ifndef ALFP_HASH_CHUNK
#define ALFP_HASH_CHUNK 512u
#endif

#endif
