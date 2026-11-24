// Aseprite Render Library
// Copyright (C) 2026  Igara Studio S.A.
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef RENDER_REF_VIEW_H_INCLUDED
#define RENDER_REF_VIEW_H_INCLUDED
#pragma once

namespace render {

// Which layers are displayed regarding reference layers (view-only
// filter, layer visibility flags are not modified).
enum class RefView {
  All,        // Normal rendering
  RefOnly,    // Only reference layers
  OthersOnly, // Every layer except reference layers
};

} // namespace render

#endif
