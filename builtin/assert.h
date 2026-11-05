/* Copyright 2016 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef PLATFORM_EC_BUILTIN_ASSERT_H_
#define PLATFORM_EC_BUILTIN_ASSERT_H_

/* Include CONFIG definitions for EC sources. */
#ifndef THIRD_PARTY
#include "common.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ASSERT(cond)

/* This collides with cstdlib, so exclude it where cstdlib is supported. */
#ifndef assert
#define assert(x...) ASSERT(x)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_EC_BUILTIN_ASSERT_H_ */
