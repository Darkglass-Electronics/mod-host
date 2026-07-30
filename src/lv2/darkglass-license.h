/*
  Copyright 2026 Robin Gareus <robin@gareus.org>
  Copyright 2026 Darkglass Electronics Oy

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/**
   @file license.h
   C header for the LV2 Darkglass License extension <http://www.darkglass.com/lv2/ns/lv2ext/license>.
*/

#ifndef LV2_DARKGLASS_LICENSE_H
#define LV2_DARKGLASS_LICENSE_H

#if defined(__has_include) && __has_include("lv2/core/lv2.h")
#include "lv2/core/lv2.h"
#else
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#endif

#define DARKGLASS_LICENSE_URI    "http://www.darkglass.com/lv2/ns/lv2ext/license"
#define DARKGLASS_LICENSE_PREFIX DARKGLASS_LICENSE_URI "#"

#define DARKGLASS_LICENSE__feature   DARKGLASS_LICENSE_PREFIX "feature"
#define DARKGLASS_LICENSE__interface DARKGLASS_LICENSE_PREFIX "interface"

#ifdef __cplusplus
extern "C" {
#endif

/** A status code for DARKGLASS_LICENSE_URI functions. */
typedef enum {
    LV2_LICENSE_SUCCESS         = 0,  /**< Plugin is licensed. */
    LV2_LICENSE_ERR_UNKNOWN     = 1,  /**< Unknown error. */
    LV2_LICENSE_ERR_UNLICENSED  = 2,  /**< Plugin is not licensed - will run in restricted/demo mode. */
    LV2_LICENSE_ERR_UNSUPPORTED = 3   /**< Plugin does not support this license API. */
} LV2_License_Status;

/**
 * Opaque pointer to host data for Darkglass_License_Feature.
 */
typedef void* LV2_License_Handle;

/**
 * Darkglass License Feature (DARKGLASS_LICENSE__feature)
 */
typedef struct _LV2_License_Feature {
    /**
     * Opaque pointer to host data.
     *
     * This MUST be passed to license() and free() whenever they are called.
     * Otherwise, it must not be interpreted in any way.
     */
    LV2_License_Handle handle;

    /**
     * Ask the host about a license file for a specific uri
     * (can be the plugin uri or a collection).
     *
     * The host will return the contents of the file, signed and encrypted,
     * or NULL if no license exists.
     *
     * The plugin must call free() on the returned data.
     *
     * @param handle Must be the handle member of this struct.
     * @param license_uri The uri for which to ask a license for.
     */
    char* (*license)(LV2_License_Handle handle, const char* license_uri);

    /**
     * Free the returned data of a license() call.
     *
     * @param license The data to be freed.
     */
    void (*free)(LV2_License_Handle handle, char* license);
} LV2_License_Feature;

/**
 * Darkglass License Interface (DARKGLASS_LICENSE__interface)
 *
 * When the plugin's extension_data is called with argument
 * DARKGLASS_LICENSE__interface, the plugin MUST return an LV2_License_Interface
 * structure, which remains valid for the lifetime of the plugin.
 *
 * The host can use the contained function pointers to query information about
 * a plugin's license. This can be used by the host to provide information to
 * the GUI (e.g. display name of licensee).
 */
typedef struct _LV2_License_Interface {
    /**
     * Get the current license status for a plugin instance.
     *
     * @see LV2_License_Status
     *
     * @param instance The LV2 instance this is a method on.
     */
    LV2_License_Status (*status)(LV2_Handle instance);

    /**
     * Get the name of the licensee for a plugin instance.
     * The caller is responsible for freeing the returned value with free().
     *
     * @param instance The LV2 instance this is a method on.
     */
    char* (*licensee)(LV2_Handle instance);

    /**
     * Free the returned data of a licensee() call.
     *
     * @param licensee The data to be freed.
     */
    void (*free)(LV2_Handle handle, char* licensee);
} LV2_License_Interface;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV2_DARKGLASS_LICENSE_H */
