/*
 * EnigmaCurry-vcv-pack
 * Copyright (C) 2021-2022 EnigmaCurry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or any later version. At your discretion, you may alternatively
 * redistribute it and/or modify it under the terms of the MIT License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and/or MIT License for more details.
 *
 * For a full copy of the GNU General Public License see the /LICENSE file.
 * For a full copy of the MIT License see the /LICENSE.MIT file.
 */
#include "plugin.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
  pluginInstance = p;

  // Add modules here
  p->addModel(modelTransport);
  p->addModel(modelLatch);

  // Any other plugin initialization may go here.
  // As an alternative, consider lazy-loading assets and lookup tables when your
  // module is created to reduce startup times of Rack.
}
