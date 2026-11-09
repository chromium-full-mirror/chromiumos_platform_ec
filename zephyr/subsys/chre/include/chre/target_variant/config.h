/*
 * Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#pragma once

#include "chre_api/chre/version.h"

// Include Zephyr's toolchain header before CHRE headers so Zephyr's
// unconditional STRINGIFY definition precedes chre/util/macros.h's
// #ifndef-guarded one.
#include <zephyr/toolchain.h>

// Platform Identifier for Google ISH
#ifndef CHRE_PLATFORM_ID
#define CHRE_PLATFORM_ID 0x476f6f676c004000
#endif

// Architecture word size (x86 32-bit Pentium)
#ifndef CHRE_32_BIT_WORD_SIZE
#define CHRE_32_BIT_WORD_SIZE 1
#endif

#ifndef EM_386
#define EM_386 3
#endif

#ifndef CHRE_LOADER_ARCH
#define CHRE_LOADER_ARCH EM_386
#endif

// Max loadable ELF segments for dynamic nanoapps
#ifndef NANOAPP_MAX_LOADABLE_SEGMENTS
#define NANOAPP_MAX_LOADABLE_SEGMENTS 8
#endif

// ChromiumOS ISH supplies its own static nanoapp list in chre.cc
#ifndef CHRE_VARIANT_SUPPLIES_STATIC_NANOAPP_LIST
#define CHRE_VARIANT_SUPPLIES_STATIC_NANOAPP_LIST
#endif

// Support AtomicUint32Ref
#ifndef CHRE_ATOMIC_UINT32_REF_ENABLED
#define CHRE_ATOMIC_UINT32_REF_ENABLED 1
#endif

// Messaging limits
#ifndef CHRE_MESSAGE_TO_HOST_MAX_SIZE
#define CHRE_MESSAGE_TO_HOST_MAX_SIZE 4000
#endif

// API versioning support
#ifndef CHRE_FIRST_SUPPORTED_API_VERSION
#define CHRE_FIRST_SUPPORTED_API_VERSION CHRE_API_VERSION_1_5
#endif

#ifndef CHRE_PATCH_VERSION
#define CHRE_PATCH_VERSION 0
#endif

// Assertions & Diagnostics
#ifndef CHRE_ASSERTIONS_ENABLED
#define CHRE_ASSERTIONS_ENABLED
#endif

#ifndef CHRE_FILENAME
#define CHRE_FILENAME __FILE__
#endif

// Logging levels
#ifndef CHRE_MINIMUM_LOG_LEVEL
#define CHRE_MINIMUM_LOG_LEVEL CHRE_LOG_LEVEL_DEBUG
#endif

#ifndef NANOAPP_MINIMUM_LOG_LEVEL
#define NANOAPP_MINIMUM_LOG_LEVEL CHRE_LOG_LEVEL_DEBUG
#endif
