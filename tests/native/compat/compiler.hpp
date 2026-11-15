/* Host tests replace all comms transport with no-op doubles; GNU packing is
 * irrelevant to these tests. This shim is never included by production builds. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(attributes)
#endif
