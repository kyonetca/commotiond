/* vim: set ts=2 expandtab: */
/**
 *       @file  plugin.h
 *      @brief  The commotiond plugin loader.
 *
 *     @author  Josh King (jheretic), jking@chambana.net
 *
 *   @internal
 *      Created  11/04/2013 10:47:37 AM
 *     Compiler  gcc/g++
 * Organization  The Open Technology Institute
 *    Copyright  Copyright (c) 2013, Josh King
 *
 * This file is part of Commotion, Copyright (c) 2013, Josh King 
 * 
 * Commotion is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * Commotion is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with Commotion.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

#ifndef _PLUGIN_H
#define _PLUGIN_H

#include <stdlib.h>
#include "obj.h"

typedef struct co_plugin_t co_plugin_t;

/**
 * @struct co_plugin_t contains file path, descriptor and name information for plugins
 */
struct co_plugin_t {
  co_obj_t _header;
  uint8_t _exttype;
  uint8_t _len;
  co_obj_t *name; /**< command name */
  co_obj_t *filename;
  co_cb_t shutdown;
  co_cb_t init;
  void *handle;
} __attribute__((packed));

/**
 * @brief shuts down and closes all plugins
 */
int co_plugins_shutdown(void);

/**
 * @brief starts all loaded plugins
 */
int co_plugins_start(void);

/**
 * @brief initializes global plugin list
 * @param index_size specifies size of index for plugins list (16 or 32 bit)
 */
int co_plugins_init(size_t index_size);

/**
 * @brief loads all plugins in specified path
 * @param dir_path directory to load plugins from
 */
int co_plugins_load(const char *dir_path);

#endif
