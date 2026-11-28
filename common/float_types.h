/*
 * float_types.h - fixed-width floating point type names required by BARR-C:2018 rule 5.4.b.i.
 *
 * Both targets used by this project (Arm Cortex-M0+ with software floating point, and the x86-64
 * host) use IEEE 754 single precision for float and double precision for double. The static
 * assertions stop the build if that is ever not true.
 */
#ifndef FLOAT_TYPES_H
#define FLOAT_TYPES_H

typedef float float32_t;
typedef double float64_t;

_Static_assert(sizeof(float32_t) == 4u, "float32_t must be 32 bits");
_Static_assert(sizeof(float64_t) == 8u, "float64_t must be 64 bits");

#endif /* FLOAT_TYPES_H */
