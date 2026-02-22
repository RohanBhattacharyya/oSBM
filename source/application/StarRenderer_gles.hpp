#pragma once

#include "StarRenderer_opengl.hpp"

namespace Star {

// Mobile GLES renderer path. Currently reuses OpenGlRenderer backend with
// GLES-compatible context setup from the mobile SDL entrypoint.
class GlesRenderer : public OpenGlRenderer {
public:
  String rendererId() const override;
};

}
