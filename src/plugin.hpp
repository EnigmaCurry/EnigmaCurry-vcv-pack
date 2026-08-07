/*
 * EnigmaCurry-vcv-pack
 * Copyright (C) 2021-2022 EnigmaCurry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the /LICENSE file.
 */
#pragma once
#include <rack.hpp>

using namespace rack;

#define TRIGGER_DURATION 1e-3f
#define LIGHT_DURATION 1e-1f

// Declare the Plugin, defined in plugin.cpp
extern Plugin *pluginInstance;

// Declare each Model, defined in each module source file
extern Model *modelTransport;
extern Model *modelLatch;
extern Model *modelPulse;
extern Model *modelRange;
extern Model *modelNegativeHarmony;
extern Model *modelEnigmaCurryMixer8;
extern Model *modelEnigmaCurryTracker;
extern Model *modelEnigmaCurryWebBridge;
extern Model *modelBrushedTest;
