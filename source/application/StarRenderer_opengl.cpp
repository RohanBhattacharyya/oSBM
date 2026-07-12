#include "StarRenderer_opengl.hpp"
#include "StarJsonExtra.hpp"
#include "StarCasting.hpp"
#include "StarLogging.hpp"

#include <cstring>
#include <cmath>

namespace Star {

size_t const MultiTextureCount = 4;

void setMobileStartupStatus(String const& status);

namespace {

#if defined(STAR_SYSTEM_IOS)
GLenum constexpr FrameBufferTextureFormat = GL_RGBA;
#else
GLenum constexpr FrameBufferTextureFormat = GL_RGB;
#endif

Vec2U framebufferTextureSize(Vec2U size, unsigned sizeDiv, float sizeMul = 1.0f) {
  sizeMul = std::max(0.05f, sizeMul);
  return {
    std::max(1u, (unsigned)((size[0] / std::max(1u, sizeDiv)) * sizeMul)),
    std::max(1u, (unsigned)((size[1] / std::max(1u, sizeDiv)) * sizeMul))
  };
}

bool isFloatTextureFormat(PixelFormat pixelFormat) {
  return pixelFormat == PixelFormat::RGB_F || pixelFormat == PixelFormat::RGBA_F;
}

bool supportsLinearFloatTextures() {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
  static bool initialized = false;
  static bool supported = false;

  if (!initialized) {
    initialized = true;
    if (auto extensions = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS)))
      supported = std::strstr(extensions, "GL_OES_texture_float_linear") != nullptr;

    if (!supported)
      Logger::warn("OpenGL ES device does not report GL_OES_texture_float_linear; using nearest-filtered float textures");
  }

  return supported;
#else
  return true;
#endif
}

TextureFiltering effectiveTextureFiltering(PixelFormat pixelFormat, TextureFiltering filtering) {
  if (filtering == TextureFiltering::Linear && isFloatTextureFormat(pixelFormat) && !supportsLinearFloatTextures())
    return TextureFiltering::Nearest;
  return filtering;
}

void setTextureFiltering(TextureFiltering filtering) {
  if (filtering == TextureFiltering::Nearest) {
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  } else {
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
}

String normalizeShaderSource(String const& sourceText, GLenum type) {
  _unused(type);
  String adjusted = sourceText.trimBeg();

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
  size_t newline = adjusted.find('\n');
  String versionLine = newline == NPos ? adjusted : adjusted.substr(0, newline);
  if (versionLine.beginsWith("#version ") && !versionLine.contains("es")) {
    if (newline == NPos)
      adjusted = "#version 300 es\n";
    else
      adjusted = String("#version 300 es\n") + adjusted.substr(newline + 1);
  }

  if (!adjusted.contains("precision ")) {
    size_t firstLineEnd = adjusted.find('\n');
    String precisionBlock = "precision highp float;\nprecision highp int;\n";
    if (firstLineEnd == NPos)
      adjusted += "\n" + precisionBlock;
    else
      adjusted = adjusted.substr(0, firstLineEnd + 1) + precisionBlock + adjusted.substr(firstLineEnd + 1);
  }
#endif

  return adjusted;
}

}

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
char const* DefaultVertexShader = R"SHADER(
#version 300 es
precision highp float;
precision highp int;

uniform vec2 textureSize0;
uniform vec2 textureSize1;
uniform vec2 textureSize2;
uniform vec2 textureSize3;
uniform vec2 screenSize;
uniform mat3 vertexTransform;

in vec2 vertexPosition;
in vec4 vertexColor;
in vec2 vertexTextureCoordinate;
in int vertexData;

out vec2 fragmentTextureCoordinate;
flat out int fragmentTextureIndex;
out vec4 fragmentColor;

void main() {
  vec2 screenPosition = (vertexTransform * vec3(vertexPosition, 1.0)).xy;
  if (((vertexData >> 3) & 0x1) == 1)
    screenPosition.x = round(screenPosition.x);
  if (((vertexData >> 4) & 0x1) == 1)
    screenPosition.y = round(screenPosition.y);
  gl_Position = vec4(screenPosition / screenSize * 2.0 - 1.0, 0.0, 1.0);
  int vertexTextureIndex = vertexData & 0x3;
  if (vertexTextureIndex == 3)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize3;
  else if (vertexTextureIndex == 2)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize2;
  else if (vertexTextureIndex == 1)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize1;
  else
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize0;

  fragmentTextureIndex = vertexTextureIndex;
  fragmentColor = vertexColor;
}
)SHADER";
#else
char const* DefaultVertexShader = R"SHADER(
#version 140

uniform vec2 textureSize0;
uniform vec2 textureSize1;
uniform vec2 textureSize2;
uniform vec2 textureSize3;
uniform vec2 screenSize;
uniform mat3 vertexTransform;

in vec2 vertexPosition;
in vec4 vertexColor;
in vec2 vertexTextureCoordinate;
in int vertexData;

out vec2 fragmentTextureCoordinate;
flat out int fragmentTextureIndex;
out vec4 fragmentColor;

void main() {
  vec2 screenPosition = (vertexTransform * vec3(vertexPosition, 1.0)).xy;
  if (((vertexData >> 3) & 0x1) == 1)
    screenPosition.x = round(screenPosition.x);
  if (((vertexData >> 4) & 0x1) == 1)
    screenPosition.y = round(screenPosition.y);
  gl_Position = vec4(screenPosition / screenSize * 2.0 - 1.0, 0.0, 1.0);
  int vertexTextureIndex = vertexData & 0x3;
  if (vertexTextureIndex == 3)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize3;
  else if (vertexTextureIndex == 2)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize2;
  else if (vertexTextureIndex == 1)
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize1;
  else
    fragmentTextureCoordinate = vertexTextureCoordinate / textureSize0;

  fragmentTextureIndex = vertexTextureIndex;
  fragmentColor = vertexColor;
}
)SHADER";
#endif

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
char const* DefaultFragmentShader = R"SHADER(
#version 300 es
precision highp float;
precision highp int;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;

in vec2 fragmentTextureCoordinate;
flat in int fragmentTextureIndex;
in vec4 fragmentColor;

out vec4 outColor;

void main() {
  vec4 texColor;
  if (fragmentTextureIndex == 3)
    texColor = texture(texture3, fragmentTextureCoordinate);
  else if (fragmentTextureIndex == 2)
    texColor = texture(texture2, fragmentTextureCoordinate);
  else if (fragmentTextureIndex == 1)
    texColor = texture(texture1, fragmentTextureCoordinate);
  else
    texColor = texture(texture0, fragmentTextureCoordinate);

  if (texColor.a <= 0.0)
    discard;

  outColor = texColor * fragmentColor;
}
)SHADER";
#else
char const* DefaultFragmentShader = R"SHADER(
#version 140

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;

in vec2 fragmentTextureCoordinate;
flat in int fragmentTextureIndex;
in vec4 fragmentColor;

out vec4 outColor;

void main() {
  vec4 texColor;
  if (fragmentTextureIndex == 3)
    texColor = texture(texture3, fragmentTextureCoordinate);
  else if (fragmentTextureIndex == 2)
    texColor = texture(texture2, fragmentTextureCoordinate);
  else if (fragmentTextureIndex == 1)
    texColor = texture(texture1, fragmentTextureCoordinate);
  else
    texColor = texture(texture0, fragmentTextureCoordinate);

  if (texColor.a <= 0.0)
    discard;

  outColor = texColor * fragmentColor;
}
)SHADER";
#endif

/*
static void GLAPIENTRY GlMessageCallback(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void* renderer) {
  if (type == GL_DEBUG_TYPE_ERROR) {
    Logger::error("GL ERROR: {}", message);
    __debugbreak();
  }
}
*/

OpenGlRenderer::OpenGlRenderer() {
  auto glewResult = glewInit();
  if (glewResult != GLEW_OK && glewResult != GLEW_ERROR_NO_GLX_DISPLAY)
    throw RendererException::format("Could not initialize GLEW: {}", (char*)glewGetErrorString(glewResult));

  if (!GLEW_VERSION_2_0)
    throw RendererException("OpenGL 2.0 not available!");

  Logger::info("OpenGL version: '{}' vendor: '{}' renderer: '{}' shader: '{}'",
      (const char*)glGetString(GL_VERSION),
      (const char*)glGetString(GL_VENDOR),
      (const char*)glGetString(GL_RENDERER),
      (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

  glClearColor(0.0, 0.0, 0.0, 1.0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  if (GLEW_VERSION_4_3) {
    //glEnable(GL_DEBUG_OUTPUT);
    //glDebugMessageCallback(GlMessageCallback, this);
  }

  m_whiteTexture = createGlTexture(Image::filled({1, 1}, Vec4B(255, 255, 255, 255), PixelFormat::RGBA32),
      TextureAddressing::Clamp,
      TextureFiltering::Nearest);
  m_immediateRenderBuffer = createGlRenderBuffer();

  loadEffectConfig("internal", JsonObject(), {{"vertex", DefaultVertexShader}, {"fragment", DefaultFragmentShader}});

  m_limitTextureGroupSize = false;
  m_useMultiTexturing = true;
  m_multiSampling = false;

  logGlErrorSummary("OpenGL errors during renderer initialization");
}

OpenGlRenderer::~OpenGlRenderer() {
  for (auto& effect : m_effects)
    glDeleteProgram(effect.second.program);

  m_frameBuffers.clear();
  logGlErrorSummary("OpenGL errors during shutdown");
}

String OpenGlRenderer::rendererId() const {
  return "OpenGL20";
}

Vec2U OpenGlRenderer::screenSize() const {
  return m_screenSize;
}

OpenGlRenderer::GlFrameBuffer::GlFrameBuffer(Json const& fbConfig) : config(fbConfig) {
  setMobileStartupStatus("Renderer: creating framebuffer texture...");
  texture = make_ref<GlLoneTexture>();
  texture->textureFiltering = TextureFiltering::Nearest;
  texture->textureAddressing = TextureAddressing::Clamp;
  texture->textureSize = {0, 0};
  glGenTextures(1, &texture->textureId);
  if (texture->textureId == 0)
    throw RendererException("Could not generate OpenGL texture for framebuffer");

  multisample = GLEW_VERSION_4_0 ? config.getUInt("multisample", 0) : 0;
  GLenum target = multisample ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  glBindTexture(target, texture->glTextureId());

  sizeDiv = config.getUInt("sizeDiv", 1);
  sizeMul = config.getFloat("sizeMul", 1.0f);
  preserve = config.getBool("preserve", false);
  premultiplied = config.getBool("premultiplied", false);
  // A premultiplied overlay buffer must carry real destination alpha
  // (coverage); the platform default framebuffer format may be GL_RGB.
  textureFormat = premultiplied ? GL_RGBA : FrameBufferTextureFormat;
  Vec2U size = framebufferTextureSize(jsonToVec2U(config.getArray("size", { 256, 256 })), sizeDiv, sizeMul);

  if (multisample)
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, multisample, GL_RGBA8, size[0], size[1], GL_TRUE);
  else {
    glTexImage2D(GL_TEXTURE_2D, 0, textureFormat, size[0], size[1], 0, textureFormat, GL_UNSIGNED_BYTE, NULL);
  }
  // Track the allocated size on the texture object: a framebuffer texture
  // drawn as an ordinary textured quad (e.g. compositeFrameBufferToCurrent)
  // derives its UV extents from texture->size(), which was left at (0,0) --
  // making any such draw sample a single corner texel.
  texture->textureSize = size;
  auto addressing = TextureAddressingNames.getLeft(config.getString("textureAddressing", "clamp"));
  auto filtering = TextureFilteringNames.getLeft(config.getString("textureFiltering", "nearest"));
  if (addressing == TextureAddressing::Clamp) {
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }
  if (!multisample) {
    if (filtering == TextureFiltering::Nearest) {
      glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
      glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
  }

  glGenFramebuffers(1, &id);
  if (!id)
    throw RendererException("Failed to create OpenGL framebuffer");

  glBindFramebuffer(GL_FRAMEBUFFER, id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture->glTextureId(), 0);

  auto framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE)
    throw RendererException("OpenGL framebuffer is not complete!");
}


OpenGlRenderer::GlFrameBuffer::~GlFrameBuffer() {
  glDeleteFramebuffers(1, &id);
  texture.reset();
}

void OpenGlRenderer::loadConfig(Json const& config) {
  setMobileStartupStatus("Renderer: resetting framebuffer configuration...");
  m_frameBuffers.clear();

  for (auto& pair : config.getObject("frameBuffers", {})) {
    setMobileStartupStatus(strf("Renderer: creating framebuffer {}...", pair.first));
    Json config = pair.second;
    config = config.set("multisample", m_multiSampling);
    Logger::info("Creating framebuffer {}", pair.first);
    m_frameBuffers[pair.first] = make_ref<GlFrameBuffer>(config);

  }
  setScreenSize(m_screenSize);
  m_config = config;
  glBindFramebuffer(GL_FRAMEBUFFER, m_screenFbo);
}

void OpenGlRenderer::loadEffectConfig(String const& name, Json const& effectConfig, StringMap<String> const& shaders) {
  setMobileStartupStatus(strf("Renderer: compiling {} shader program...", name));
  if (auto effect = m_effects.ptr(name)) {
    Logger::info("Reloading OpenGL effect {}", name);
    glDeleteProgram(effect->program);
    m_effects.erase(name);
  }

  GLint status = 0;
  char logBuffer[1024];

  auto compileShaderSource = [&](GLenum type, String const& sourceText, char const* shaderName) -> GLuint {
    String shaderSource = normalizeShaderSource(sourceText, type);
    char const* sourcePtr = shaderSource.utf8Ptr();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &sourcePtr, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
      glGetShaderInfoLog(shader, sizeof(logBuffer), NULL, logBuffer);
      glDeleteShader(shader);
      throw RendererException(strf("Failed to compile {} shader: {}\n", shaderName, logBuffer));
    }

    return shader;
  };

  auto compileShader = [&](GLenum type, String const& name) -> GLuint {
    auto const* source = shaders.ptr(name);
    if (!source)
      return 0;
    return compileShaderSource(type, *source, name.utf8Ptr());
  };

  GLuint vertexShader = 0, fragmentShader = 0;
  try {
    vertexShader = compileShader(GL_VERTEX_SHADER, "vertex");
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, "fragment");
  }
  catch (RendererException const& e) {
    Logger::error("Shader compile error, using default: {}", e.what());
    if (vertexShader) glDeleteShader(vertexShader);
    if (fragmentShader) glDeleteShader(fragmentShader);
    vertexShader = compileShaderSource(GL_VERTEX_SHADER, DefaultVertexShader, "default-vertex");
    fragmentShader = compileShaderSource(GL_FRAGMENT_SHADER, DefaultFragmentShader, "default-fragment");
  }

  GLuint program = glCreateProgram();

  if (vertexShader)
    glAttachShader(program, vertexShader);
  if (fragmentShader)
    glAttachShader(program, fragmentShader);
  glLinkProgram(program);

  if (vertexShader)
    glDeleteShader(vertexShader);
  if (fragmentShader)
    glDeleteShader(fragmentShader);

  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    glGetProgramInfoLog(program, sizeof(logBuffer), NULL, logBuffer);
    glDeleteProgram(program);
    throw RendererException(strf("Failed to link program: {}\n", logBuffer));
  }

  glUseProgram(m_program = program);

  auto& effect = m_effects.emplace(name, Effect()).first->second;
  effect.program = m_program;
  effect.config = effectConfig;
  effect.includeVBTextures = effectConfig.getBool("includeVBTextures",true);
  m_currentEffect = &effect;
  setupGlUniforms(effect, m_screenSize);

  for (auto const& p : effectConfig.getObject("effectParameters", {})) {
    EffectParameter effectParameter;

    effectParameter.parameterUniform = glGetUniformLocation(m_program, p.second.getString("uniform").utf8Ptr());
    if (effectParameter.parameterUniform == -1) {
      Logger::warn("OpenGL20 effect parameter '{}' in effect '{}' has no associated uniform, skipping", p.first, name);
    } else {
      String type = p.second.getString("type");
      if (type == "bool") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<bool>();
      } else if (type == "int") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<int>();
      } else if (type == "float") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<float>();
      } else if (type == "vec2") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<Vec2F>();
      } else if (type == "vec3") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<Vec3F>();
      } else if (type == "vec4") {
        effectParameter.parameterType = RenderEffectParameter::typeIndexOf<Vec4F>();
      } else {
        throw RendererException::format("Unrecognized effect parameter type '{}'", type);
      }

      if (p.second.getBool("scriptable",false)) {
        if (Json def = p.second.get("default", {})) {
          if (type == "bool") {
            effectParameter.parameterValue = (RenderEffectParameter)def.toBool();
          } else if (type == "int") {
            effectParameter.parameterValue = (RenderEffectParameter)(int)def.toInt();
          } else if (type == "float") {
            effectParameter.parameterValue = (RenderEffectParameter)def.toFloat();
          } else if (type == "vec2") {
            effectParameter.parameterValue = (RenderEffectParameter)jsonToVec2F(def);
          } else if (type == "vec3") {
            effectParameter.parameterValue = (RenderEffectParameter)jsonToVec3F(def);
          } else if (type == "vec4") {
            effectParameter.parameterValue = (RenderEffectParameter)jsonToVec4F(def);
          }
        }
        effect.scriptables[p.first] = effectParameter;
      } else {
        effect.parameters[p.first] = effectParameter;
        if (Json def = p.second.get("default", {})) {
          if (type == "bool") {
            setEffectParameter(p.first, def.toBool());
          } else if (type == "int") {
            setEffectParameter(p.first, (int)def.toInt());
          } else if (type == "float") {
            setEffectParameter(p.first, def.toFloat());
          } else if (type == "vec2") {
            setEffectParameter(p.first, jsonToVec2F(def));
          } else if (type == "vec3") {
            setEffectParameter(p.first, jsonToVec3F(def));
          } else if (type == "vec4") {
            setEffectParameter(p.first, jsonToVec4F(def));
          }
        }
      }
    }
  }

  // Assign each texture parameter a texture unit starting with MultiTextureCount, the first
  // few texture units are used by the primary textures being drawn.  Currently,
  // maximum texture units are not checked.
  unsigned parameterTextureUnit = effect.includeVBTextures ? MultiTextureCount : 0;

  for (auto const& p : effectConfig.getObject("effectTextures", {})) {
    EffectTexture effectTexture;
    effectTexture.textureUniform = glGetUniformLocation(m_program, p.second.getString("textureUniform").utf8Ptr());
    if (effectTexture.textureUniform == -1) {
      Logger::warn("OpenGL20 effect parameter '{}' has no associated uniform, skipping", p.first);
    } else {
        effectTexture.textureUnit = parameterTextureUnit++;
        glUniform1i(effectTexture.textureUniform, effectTexture.textureUnit);

        effectTexture.textureAddressing = TextureAddressingNames.getLeft(p.second.getString("textureAddressing", "clamp"));
        effectTexture.textureFiltering = TextureFilteringNames.getLeft(p.second.getString("textureFiltering", "nearest"));
        if (auto tsu = p.second.optString("textureSizeUniform")) {
          effectTexture.textureSizeUniform = glGetUniformLocation(m_program, tsu->utf8Ptr());
          if (effectTexture.textureSizeUniform == -1)
            Logger::warn("OpenGL20 effect parameter '{}' has textureSizeUniform '{}' with no associated uniform", p.first, *tsu);
        }

      effect.textures[p.first] = effectTexture;
    }
  }

  if (DebugEnabled)
    logGlErrorSummary("OpenGL errors setting effect config");
}

void OpenGlRenderer::setEffectParameter(String const& parameterName, RenderEffectParameter const& value) {
  auto ptr = m_currentEffect->parameters.ptr(parameterName);
  if (!ptr || (ptr->parameterValue && *ptr->parameterValue == value))
    return;

  if (ptr->parameterType != value.typeIndex())
    throw RendererException::format("OpenGlRenderer::setEffectParameter '{}' parameter type mismatch", parameterName);

  flushImmediatePrimitives();

  if (auto v = value.ptr<bool>())
    glUniform1i(ptr->parameterUniform, *v);
  else if (auto v = value.ptr<int>())
    glUniform1i(ptr->parameterUniform, *v);
  else if (auto v = value.ptr<float>())
    glUniform1f(ptr->parameterUniform, *v);
  else if (auto v = value.ptr<Vec2F>())
    glUniform2f(ptr->parameterUniform, (*v)[0], (*v)[1]);
  else if (auto v = value.ptr<Vec3F>())
    glUniform3f(ptr->parameterUniform, (*v)[0], (*v)[1], (*v)[2]);
  else if (auto v = value.ptr<Vec4F>())
    glUniform4f(ptr->parameterUniform, (*v)[0], (*v)[1], (*v)[2], (*v)[3]);

  ptr->parameterValue = value;
}

void OpenGlRenderer::setEffectScriptableParameter(String const& effectName, String const& parameterName, RenderEffectParameter const& value) {
  auto find = m_effects.find(effectName);
  if (find == m_effects.end())
    return;

  Effect& effect = find->second;
  
  auto ptr = effect.scriptables.ptr(parameterName);
  if (!ptr || (ptr->parameterValue && *ptr->parameterValue == value))
    return;

  if (ptr->parameterType != value.typeIndex())
    throw RendererException::format("OpenGlRenderer::setEffectScriptableParameter '{}' parameter type mismatch", parameterName);

  ptr->parameterValue = value;
}

Maybe<RenderEffectParameter> OpenGlRenderer::getEffectScriptableParameter(String const& effectName, String const& parameterName) {
  auto find = m_effects.find(effectName);
  if (find == m_effects.end())
    return {};

  Effect& effect = find->second;

  auto ptr = effect.scriptables.ptr(parameterName);
  if (!ptr)
    return {};
  
  return ptr->parameterValue;
}
Maybe<VariantTypeIndex> OpenGlRenderer::getEffectScriptableParameterType(String const& effectName, String const& parameterName) {
  auto find = m_effects.find(effectName);
  if (find == m_effects.end())
    return {};

  Effect& effect = find->second;

  auto ptr = effect.scriptables.ptr(parameterName);
  if (!ptr)
    return {};
  
  return ptr->parameterType;
}

void OpenGlRenderer::setEffectTexture(String const& textureName, ImageView const& image) {
  auto ptr = m_currentEffect->textures.ptr(textureName);
  if (!ptr)
    return;

  flushImmediatePrimitives();

  if (!ptr->textureValue || ptr->textureValue->textureId == 0) {
    ptr->textureValue = createGlTexture(image, ptr->textureAddressing, ptr->textureFiltering);
  } else {
    glBindTexture(GL_TEXTURE_2D, ptr->textureValue->textureId);
    ptr->textureValue->textureSize = image.size;
    ptr->textureValue->textureFiltering = effectiveTextureFiltering(image.format, ptr->textureFiltering);
    setTextureFiltering(ptr->textureValue->textureFiltering);
    uploadTextureImage(image.format, image.size, image.data);
  }

  if (ptr->textureSizeUniform != -1) {
    auto textureSize = ptr->textureValue->glTextureSize();
    glUniform2f(ptr->textureSizeUniform, textureSize[0], textureSize[1]);
  }
}

bool OpenGlRenderer::switchEffectConfig(String const& name) {
  flushImmediatePrimitives();
  auto find = m_effects.find(name);
  if (find == m_effects.end())
    return false;

  Effect& effect = find->second;
  if (m_currentEffect == &effect)
    return true;

  if (auto blitFrameBufferId = effect.config.optString("blitFrameBuffer"))
    blitGlFrameBuffer(getGlFrameBuffer(*blitFrameBufferId));
#ifdef STAR_SYSTEM_SWITCH
  else {
    static int64_t s_noBlitSwitches = 0;
    if (++s_noBlitSwitches % 300 == 0)
      Logger::info("[blit] {} effect switches without blitFrameBuffer (to '{}')", s_noBlitSwitches, name);
  }
#endif

  auto effectScreenSize = m_screenSize;
  if (auto frameBufferId = effect.config.optString("frameBuffer")) {
    auto buf = getGlFrameBuffer(*frameBufferId);
    switchGlFrameBuffer(buf);
    // Shader screenSize stays the LOGICAL size (vertices are authored in full screen
    // pixels) so the world maps to the full NDC range; only the physical viewport
    // shrinks by sizeMul, so the same view rasterizes into fewer pixels and is then
    // upscaled on blit -- a fill-rate win with the field of view preserved.
    effectScreenSize = m_screenSize / (buf->sizeDiv);
    Vec2U fbViewport = framebufferTextureSize(m_screenSize, buf->sizeDiv, buf->sizeMul);
    // Intermediate FBOs are canvas-sized with no screen offset; the offset
    // only applies to the final blit onto the physical screen FBO.
    glViewport(0, 0, fbViewport[0], fbViewport[1]);
  } else {
    m_currentFrameBuffer.reset();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_screenFbo);
    // Restore the screen viewport (canvas positioned at its safe-area offset).
    glViewport(m_screenOffset[0], m_screenOffset[1], m_screenViewportSize[0], m_screenViewportSize[1]);
  }

  glUseProgram(m_program = effect.program);
  setupGlUniforms(effect, effectScreenSize);
  m_currentEffect = &effect;

  setEffectParameter("vertexRounding", m_multiSampling > 0);
  if (auto fbts = effect.config.optArray("frameBufferTextures")) {
    for (auto const& fbt : *fbts) {
      if (auto frameBufferId = fbt.optString("framebuffer")) {
        auto textureUniform = fbt.getString("texture");
        auto ptr = m_currentEffect->textures.ptr(textureUniform);
        if (ptr) {
          if (!ptr->textureValue || ptr->textureValue->textureId == 0) {  
            auto texture = getGlFrameBuffer(*frameBufferId)->texture;
            ptr->textureValue = texture;
            if (ptr->textureSizeUniform != -1) {
              auto textureSize = ptr->textureValue->glTextureSize();
              glUniform2f(ptr->textureSizeUniform, textureSize[0], textureSize[1]);
            }
          }
        }
      }
    }
  }
  return true;
}

bool OpenGlRenderer::switchFrameBuffer(String const& id) {
  auto ptr = m_frameBuffers.ptr(id);
  if (!ptr)
    return false;
  if (m_currentFrameBuffer == *ptr)
    return true;
  flushImmediatePrimitives();
  switchGlFrameBuffer(*ptr);
  // Same viewport rule as switchEffectConfig's framebuffer path: the shader
  // screenSize uniform keeps the LOGICAL size (vertices are authored in full
  // screen pixels), only the physical viewport shrinks by sizeDiv/sizeMul, so
  // the same view rasterizes into fewer pixels.
  Vec2U fbViewport = framebufferTextureSize(m_screenSize, (*ptr)->sizeDiv, (*ptr)->sizeMul);
  glViewport(0, 0, fbViewport[0], fbViewport[1]);
  return true;
}

void OpenGlRenderer::blitFrameBufferToCurrent(String const& id) {
  auto ptr = m_frameBuffers.ptr(id);
  if (!ptr || !m_currentFrameBuffer)
    return;
  flushImmediatePrimitives();
  Vec2U srcSize = framebufferTextureSize(m_screenSize, (*ptr)->sizeDiv, (*ptr)->sizeMul);
  Vec2U dstSize = framebufferTextureSize(m_screenSize, m_currentFrameBuffer->sizeDiv, m_currentFrameBuffer->sizeMul);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (*ptr)->id);
  glBlitFramebuffer(
    0, 0, srcSize[0], srcSize[1],
    0, 0, dstSize[0], dstSize[1],
    GL_COLOR_BUFFER_BIT, srcSize == dstSize ? GL_NEAREST : GL_LINEAR
  );
}

void OpenGlRenderer::compositeFrameBufferToCurrent(String const& id) {
  auto ptr = m_frameBuffers.ptr(id);
  if (!ptr)
    return;
  flushImmediatePrimitives();
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  render(renderTexturedRect(TexturePtr((*ptr)->texture), RectF::withSize({}, Vec2F(m_screenSize))));
  flushImmediatePrimitives();
  if (m_currentFrameBuffer && m_currentFrameBuffer->premultiplied)
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  else
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void OpenGlRenderer::clearCurrentFrameBuffer() {
  flushImmediatePrimitives();
  if (m_scissorRect)
    glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  if (m_scissorRect)
    glEnable(GL_SCISSOR_TEST);
}

void OpenGlRenderer::switchToDefaultFrameBuffer() {
  flushImmediatePrimitives();
  m_currentFrameBuffer.reset();
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_screenFbo);
  glViewport(m_screenOffset[0], m_screenOffset[1], m_screenViewportSize[0], m_screenViewportSize[1]);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void OpenGlRenderer::setScissorRect(Maybe<RectI> const& scissorRect) {
  if (scissorRect == m_scissorRect)
    return;

  flushImmediatePrimitives();

  // Scissor state changed mid-recording: primitives submitted from here on
  // belong to a new segment carrying the new scissor state.
  if (m_recording)
    m_recordingSegments.append(RecordedSegment{scissorRect, {}});

  m_scissorRect = scissorRect;
  if (m_scissorRect) {
    glEnable(GL_SCISSOR_TEST);
    float xScale = m_screenSize[0] ? (float)m_screenViewportSize[0] / (float)m_screenSize[0] : 1.0f;
    float yScale = m_screenSize[1] ? (float)m_screenViewportSize[1] / (float)m_screenSize[1] : 1.0f;
    glScissor(
      (GLint)std::round(m_scissorRect->xMin() * xScale) + (GLint)m_screenOffset[0],
      (GLint)std::round(m_scissorRect->yMin() * yScale) + (GLint)m_screenOffset[1],
      (GLsizei)std::round(m_scissorRect->width() * xScale),
      (GLsizei)std::round(m_scissorRect->height() * yScale)
    );
  } else {
    glDisable(GL_SCISSOR_TEST);
  }
}

TexturePtr OpenGlRenderer::createTexture(Image const& texture, TextureAddressing addressing, TextureFiltering filtering) {
  return createGlTexture(texture, addressing, filtering);
}

void OpenGlRenderer::setSizeLimitEnabled(bool enabled) {
  m_limitTextureGroupSize = enabled;
}

void OpenGlRenderer::setMultiTexturingEnabled(bool enabled) {
  m_useMultiTexturing = enabled;
}

void OpenGlRenderer::setMultiSampling(unsigned multiSampling) {
  if (m_multiSampling == multiSampling)
    return;

  m_multiSampling = multiSampling;
  if (m_multiSampling) {
    glEnable(GL_MULTISAMPLE);
#if !defined(STAR_SYSTEM_IOS)
    glEnable(GL_SAMPLE_SHADING);
    glMinSampleShading(1.f);
#endif
  } else {
#if !defined(STAR_SYSTEM_IOS)
    glMinSampleShading(0.f);
    glDisable(GL_SAMPLE_SHADING);
#endif
    glDisable(GL_MULTISAMPLE);
  }
  loadConfig(m_config);
}

TextureGroupPtr OpenGlRenderer::createTextureGroup(TextureGroupSize textureSize, TextureFiltering filtering) {
  int maxTextureSize;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  maxTextureSize = min(maxTextureSize, (2 << 14));
  // Large texture sizes are not always supported
  if (textureSize == TextureGroupSize::Large && (m_limitTextureGroupSize || maxTextureSize < 4096))
    textureSize = TextureGroupSize::Medium;

  unsigned atlasNumCells;
  if (textureSize == TextureGroupSize::Large)
    atlasNumCells = 256;
  else if (textureSize == TextureGroupSize::Medium)
    atlasNumCells = 128;
  else // TextureGroupSize::Small
    atlasNumCells = 64;

  Logger::info("detected supported OpenGL texture size {}, using atlasNumCells {}", maxTextureSize, atlasNumCells);

  auto glTextureGroup = make_shared<GlTextureGroup>(atlasNumCells);
  glTextureGroup->textureAtlasSet.textureFiltering = filtering;
  m_liveTextureGroups.append(glTextureGroup);
  return glTextureGroup;
}

RenderBufferPtr OpenGlRenderer::createRenderBuffer() {
  return createGlRenderBuffer();
}

List<RenderPrimitive>& OpenGlRenderer::immediatePrimitives() {
  return m_immediatePrimitives;
}

void OpenGlRenderer::render(RenderPrimitive primitive) {
  m_immediatePrimitives.append(std::move(primitive));
}

void OpenGlRenderer::renderBuffer(RenderBufferPtr const& renderBuffer, Mat3F const& transformation) {
  flushImmediatePrimitives();
  renderGlBuffer(*convert<GlRenderBuffer>(renderBuffer.get()), transformation);
}

void OpenGlRenderer::flush(Mat3F const& transformation) {
  flushImmediatePrimitives(transformation);
}

void OpenGlRenderer::setScreenSize(Vec2U screenSize) {
  m_screenSize = screenSize;
  if (m_screenViewportSize[0] == 0 || m_screenViewportSize[1] == 0)
    m_screenViewportSize = screenSize;
  glViewport(m_screenOffset[0], m_screenOffset[1], m_screenViewportSize[0], m_screenViewportSize[1]);
  glUniform2f(m_screenSizeUniform, m_screenSize[0], m_screenSize[1]);

  for (auto& frameBuffer : m_frameBuffers) {
    unsigned sizeDiv = frameBuffer.second->sizeDiv;
    Vec2U textureSize = framebufferTextureSize(m_screenSize, sizeDiv, frameBuffer.second->sizeMul);
    if (unsigned multisample = frameBuffer.second->multisample) {
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, frameBuffer.second->texture->glTextureId());
      glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, multisample, GL_RGBA8, textureSize[0], textureSize[1], GL_TRUE);
    } else {
      glBindTexture(GL_TEXTURE_2D, frameBuffer.second->texture->glTextureId());
      GLenum fbFormat = frameBuffer.second->textureFormat ? frameBuffer.second->textureFormat : FrameBufferTextureFormat;
      glTexImage2D(GL_TEXTURE_2D, 0, fbFormat, textureSize[0], textureSize[1], 0, fbFormat, GL_UNSIGNED_BYTE, NULL);
      frameBuffer.second->texture->textureSize = textureSize;
    }
  }
}

void OpenGlRenderer::setScreenViewportSize(Vec2U viewportSize) {
  m_screenViewportSize = viewportSize;
  glViewport(m_screenOffset[0], m_screenOffset[1], m_screenViewportSize[0], m_screenViewportSize[1]);
}

void OpenGlRenderer::startFrame() {
  if (m_scissorRect)
    glDisable(GL_SCISSOR_TEST);

  glViewport(m_screenOffset[0], m_screenOffset[1], m_screenViewportSize[0], m_screenViewportSize[1]);

  for (auto& frameBuffer : m_frameBuffers) {
    if (!frameBuffer.second->preserve) {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer.second->id);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    frameBuffer.second->blitted = false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, m_screenFbo);

  glClear(GL_COLOR_BUFFER_BIT);

  if (m_scissorRect)
    glEnable(GL_SCISSOR_TEST);
}

void OpenGlRenderer::finishFrame() {
  flushImmediatePrimitives();

#ifdef STAR_SYSTEM_SWITCH
  // Diagnostic self-screenshot of the fully composed frame (world + HUD):
  // host-side captures of the emulator window can be black under Wayland
  // direct scanout, so this is the reliable way to verify visual output.
  // Ryujinx's glReadPixels row pitch is buggy (only ~width/4 of each row is
  // valid) but colors and shapes remain judgeable.
  {
    // Poll via the CONTENT of an always-existing control file: checking for
    // a missing file on Switch means opendir+readdir (libnx stat aborts on
    // missing paths), and libnx fsdev path handling races other threads'
    // file IO -- an opendir poll here intermittently crashed in strchr.
    // Reading an existing file with fopen is safe. Trigger externally with:
    //   echo 1 > /switch/oSBM/screenshot.ctl
    static uint64_t s_ssCheckCounter = 0;
    static int s_ssCtlState = -1; // -1 unchecked, 0 ready, 1 unavailable
    if (s_ssCtlState == -1 && ++s_ssCheckCounter >= 300) {
      // One-time setup, done well after boot so the SD devoptab is stable:
      // ensure the control file exists (single opendir-based existence check).
      s_ssCtlState = 1;
      try {
        if (!File::isFile("/switch/oSBM/screenshot.ctl"))
          File::writeFile("0", 1, "/switch/oSBM/screenshot.ctl");
        s_ssCtlState = 0;
      } catch (std::exception const&) {}
    }
    auto screenshotRequested = [&]() -> bool {
      if (s_ssCtlState != 0)
        return false;
      try {
        auto contents = File::readFile("/switch/oSBM/screenshot.ctl");
        if (!contents.empty() && contents[0] == '1') {
          File::writeFile("0", 1, "/switch/oSBM/screenshot.ctl");
          return true;
        }
      } catch (std::exception const&) {}
      return false;
    };
    if (s_ssCtlState == 0 && s_ssCheckCounter++ % 60 == 0 && screenshotRequested()) {
      Vec2U wsize = m_screenSize;
      Image img(wsize, PixelFormat::RGBA32);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, m_screenFbo);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, wsize[0], wsize[1], GL_RGBA, GL_UNSIGNED_BYTE, img.data());
      try {
        img.writePng(File::open("/switch/oSBM/screenshot.png", IOMode::Write));
        // Also dump the named intermediate framebuffers, to localize which
        // stage lost its content if the composed frame looks wrong.
        for (auto const& fbName : {String("main"), String("hud"), String("background")}) {
          if (auto fb = m_frameBuffers.ptr(fbName)) {
            Vec2U fbSize = framebufferTextureSize(m_screenSize, (*fb)->sizeDiv, (*fb)->sizeMul);
            Image fbImg(fbSize, PixelFormat::RGBA32);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (*fb)->id);
            glReadPixels(0, 0, fbSize[0], fbSize[1], GL_RGBA, GL_UNSIGNED_BYTE, fbImg.data());
            fbImg.writePng(File::open(strf("/switch/oSBM/screenshot_{}.png", fbName), IOMode::Write));
          }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_screenFbo);
        Logger::info("Diagnostic screenshot written ({}x{})", wsize[0], wsize[1]);
      } catch (std::exception const& e) {
        Logger::error("Diagnostic screenshot failed: {}", e.what());
      }
    }
  }
#endif

  // Atlas compression is pure memory housekeeping (defragmenting atlas
  // pages), but each pass SCANS every texture in the group -- measured at
  // several ms/frame with a full world's sprite set loaded. Run it on a
  // fraction of frames; a few frames of defrag latency is invisible.
#ifdef STAR_SYSTEM_FAMILY_MOBILE
  static unsigned s_compressionCounter = 0;
  bool runCompression = (++s_compressionCounter % 8) == 0;
#else
  bool runCompression = true;
#endif
  if (runCompression) {
    // Make sure that the immediate render buffer doesn't needlessly lock
    // textures from being compressed (only matters when compression runs).
    List<RenderPrimitive> empty;
    m_immediateRenderBuffer->set(empty);
    filter(m_liveTextureGroups, [](auto const& p) {
          unsigned const CompressionsPerFrame = 1;

          if (!p.unique() || p->textureAtlasSet.totalTextures() > 0) {
            p->textureAtlasSet.compressionPass(CompressionsPerFrame);
            return true;
          }

          return false;
        });
  }

  // Blit if another shader hasn't
  glBindFramebuffer(GL_FRAMEBUFFER, m_screenFbo);

  if (DebugEnabled)
    logGlErrorSummary("OpenGL errors this frame");
}

OpenGlRenderer::GlTextureAtlasSet::GlTextureAtlasSet(unsigned atlasNumCells)
  : TextureAtlasSet(16, atlasNumCells) {}

GLuint OpenGlRenderer::GlTextureAtlasSet::createAtlasTexture(Vec2U const& size, PixelFormat pixelFormat) {
  GLuint glTextureId;
  glGenTextures(1, &glTextureId);
  if (glTextureId == 0)
    throw RendererException("Could not generate texture in OpenGlRenderer::TextureGroup::createAtlasTexture()");

  glBindTexture(GL_TEXTURE_2D, glTextureId);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (textureFiltering == TextureFiltering::Nearest) {
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  } else {
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }

  uploadTextureImage(pixelFormat, size, nullptr);
  return glTextureId;
}

void OpenGlRenderer::GlTextureAtlasSet::destroyAtlasTexture(GLuint const& glTexture) {
  glDeleteTextures(1, &glTexture);
}

void OpenGlRenderer::GlTextureAtlasSet::copyAtlasPixels(
    GLuint const& glTexture, Vec2U const& bottomLeft, Image const& image) {
  glBindTexture(GL_TEXTURE_2D, glTexture);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  GLenum format;
  auto pixelFormat = image.pixelFormat();
  if (pixelFormat == PixelFormat::RGB24)
    format = GL_RGB;
  else if (pixelFormat == PixelFormat::RGBA32)
    format = GL_RGBA;
  else if (pixelFormat == PixelFormat::BGR24)
    format = GL_BGR;
  else if (pixelFormat == PixelFormat::BGRA32)
    format = GL_BGRA;
  else
    throw RendererException("Unsupported texture format in OpenGlRenderer::TextureGroup::copyAtlasPixels");

  glTexSubImage2D(GL_TEXTURE_2D, 0, bottomLeft[0], bottomLeft[1], image.width(), image.height(), format, GL_UNSIGNED_BYTE, image.data());
}

OpenGlRenderer::GlTextureGroup::GlTextureGroup(unsigned atlasNumCells)
  : textureAtlasSet(atlasNumCells) {}

OpenGlRenderer::GlTextureGroup::~GlTextureGroup() {
  textureAtlasSet.reset();
}

TextureFiltering OpenGlRenderer::GlTextureGroup::filtering() const {
  return textureAtlasSet.textureFiltering;
}

TexturePtr OpenGlRenderer::GlTextureGroup::create(Image const& texture) {
  // If the image is empty, or would not fit in the texture atlas with border
  // pixels, just create a regular texture
  Vec2U atlasTextureSize = textureAtlasSet.atlasTextureSize();
  if (texture.empty() || texture.width() + 2 > atlasTextureSize[0] || texture.height() + 2 > atlasTextureSize[1])
    return createGlTexture(texture, TextureAddressing::Clamp, textureAtlasSet.textureFiltering);

  auto glGroupedTexture = make_ref<GlGroupedTexture>();
  glGroupedTexture->parentGroup = shared_from_this();
  glGroupedTexture->parentAtlasTexture = textureAtlasSet.addTexture(texture);

  return glGroupedTexture;
}

OpenGlRenderer::GlGroupedTexture::~GlGroupedTexture() {
  if (parentAtlasTexture)
    parentGroup->textureAtlasSet.freeTexture(parentAtlasTexture);
}

Vec2U OpenGlRenderer::GlGroupedTexture::size() const {
  return parentAtlasTexture->imageSize();
}

TextureFiltering OpenGlRenderer::GlGroupedTexture::filtering() const {
  return parentGroup->filtering();
}

TextureAddressing OpenGlRenderer::GlGroupedTexture::addressing() const {
  return TextureAddressing::Clamp;
}

GLuint OpenGlRenderer::GlGroupedTexture::glTextureId() const {
  return parentAtlasTexture->atlasTexture();
}

Vec2U OpenGlRenderer::GlGroupedTexture::glTextureSize() const {
  return parentGroup->textureAtlasSet.atlasTextureSize();
}

Vec2U OpenGlRenderer::GlGroupedTexture::glTextureCoordinateOffset() const {
  return parentAtlasTexture->atlasTextureCoordinates().min();
}

void OpenGlRenderer::GlGroupedTexture::incrementBufferUseCount() {
  if (bufferUseCount == 0)
    parentAtlasTexture->setLocked(true);
  ++bufferUseCount;
}

void OpenGlRenderer::GlGroupedTexture::decrementBufferUseCount() {
  starAssert(bufferUseCount != 0);
  if (bufferUseCount == 1)
    parentAtlasTexture->setLocked(false);
  --bufferUseCount;
}

OpenGlRenderer::GlLoneTexture::~GlLoneTexture() {
  if (textureId != 0)
    glDeleteTextures(1, &textureId);
}

Vec2U OpenGlRenderer::GlLoneTexture::size() const {
  return textureSize;
}

TextureFiltering OpenGlRenderer::GlLoneTexture::filtering() const {
  return textureFiltering;
}

TextureAddressing OpenGlRenderer::GlLoneTexture::addressing() const {
  return textureAddressing;
}

GLuint OpenGlRenderer::GlLoneTexture::glTextureId() const {
  return textureId;
}

Vec2U OpenGlRenderer::GlLoneTexture::glTextureSize() const {
  return textureSize;
}

Vec2U OpenGlRenderer::GlLoneTexture::glTextureCoordinateOffset() const {
  return Vec2U();
}

OpenGlRenderer::GlRenderBuffer::GlRenderBuffer() {
  glGenVertexArrays(1, &vertexArray);
}

OpenGlRenderer::GlRenderBuffer::~GlRenderBuffer() {
  for (auto const& texture : usedTextures) {
    if (auto gt = as<GlGroupedTexture>(texture.get()))
      gt->decrementBufferUseCount();
  }
  for (auto const& vb : vertexBuffers)
    glDeleteBuffers(1, &vb.vertexBuffer);
  glDeleteVertexArrays(1, &vertexArray);
}

void OpenGlRenderer::GlRenderBuffer::set(List<RenderPrimitive>& primitives) {
  for (auto const& texture : usedTextures) {
    if (auto gt = as<GlGroupedTexture>(texture.get()))
      gt->decrementBufferUseCount();
  }
  usedTextures.clear();

  auto oldVertexBuffers = take(vertexBuffers);

  List<GLuint> currentTextures;
  List<Vec2U> currentTextureSizes;
  size_t currentVertexCount = 0;
  glBindVertexArray(vertexArray);
  auto finishCurrentBuffer = [&]() {
    if (currentVertexCount > 0) {
      GlVertexBuffer vb;
      for (size_t i = 0; i < currentTextures.size(); ++i) {
        vb.textures.append(GlVertexBufferTexture{currentTextures[i], currentTextureSizes[i]});
      }
      vb.vertexCount = currentVertexCount;
      if (!oldVertexBuffers.empty()) {
        auto oldVb = oldVertexBuffers.takeLast();
        vb.vertexBuffer = oldVb.vertexBuffer;
        glBindBuffer(GL_ARRAY_BUFFER, vb.vertexBuffer);
        if (oldVb.vertexCount >= vb.vertexCount)
          glBufferSubData(GL_ARRAY_BUFFER, 0, accumulationBuffer.size(), accumulationBuffer.ptr());
        else
          glBufferData(GL_ARRAY_BUFFER, accumulationBuffer.size(), accumulationBuffer.ptr(), GL_STREAM_DRAW);
      } else {
        glGenBuffers(1, &vb.vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, vb.vertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, accumulationBuffer.size(), accumulationBuffer.ptr(), GL_STREAM_DRAW);
      }

      vertexBuffers.emplace_back(std::move(vb));

      currentTextures.clear();
      currentTextureSizes.clear();
      accumulationBuffer.clear();
      currentVertexCount = 0;
    }
  };

  auto textureCount = useMultiTexturing ? MultiTextureCount : 1;
  auto addCurrentTexture = [&](TexturePtr texture) -> pair<uint8_t, Vec2F> {
    if (!texture)
      texture = whiteTexture;

    auto glTexture = as<GlTexture>(texture.get());
    GLuint glTextureId = glTexture->glTextureId();

    auto textureIndex = currentTextures.indexOf(glTextureId);
    if (textureIndex == NPos) {
      if (currentTextures.size() >= textureCount)
        finishCurrentBuffer();

      textureIndex = currentTextures.size();
      currentTextures.append(glTextureId);
      currentTextureSizes.append(glTexture->glTextureSize());
    }

    if (auto gt = as<GlGroupedTexture>(texture.get()))
      gt->incrementBufferUseCount();
    usedTextures.add(std::move(texture));

    return {float(textureIndex), Vec2F(glTexture->glTextureCoordinateOffset())};
  };

  auto appendBufferVertex = [&](RenderVertex const& v, uint8_t textureIndex, Vec2F textureCoordinateOffset, RenderVertex const& prev, RenderVertex const& next) {
    size_t off = accumulationBuffer.size();
    accumulationBuffer.resize(accumulationBuffer.size() + sizeof(GlRenderVertex));
    GlRenderVertex& glv = *(GlRenderVertex*)(accumulationBuffer.ptr() + off);
    glv.pos = v.screenCoordinate;
    glv.uv = v.textureCoordinate + textureCoordinateOffset;
    glv.color = v.color;
    glv.pack.vars.textureIndex = textureIndex;
    glv.pack.vars.fullbright = v.param1 > 0.0f;
    // Tell the vertex shader to round to the nearest pixel if the vertices form a straight
    // edge, to ensure sharpness with supersampling. If we rounded *all* vertex positions,
    // it'd cause slight visual issues with sprites rotating around a point.
    glv.pack.vars.rX = min(abs(glv.pos.x() - prev.screenCoordinate.x()), abs(glv.pos.x() - next.screenCoordinate.x())) < 0.001f;
    glv.pack.vars.rY = min(abs(glv.pos.y() - prev.screenCoordinate.y()), abs(glv.pos.y() - next.screenCoordinate.y())) < 0.001f;
    glv.pack.vars.unused = 0;
    ++currentVertexCount;
    return glv;
  };

  uint8_t textureIndex = 0;
  Vec2F textureOffset = {};
  for (auto& primitive : primitives) {
    if (auto tri = primitive.ptr<RenderTriangle>()) {
      tie(textureIndex, textureOffset) = addCurrentTexture(std::move(tri->texture));

      appendBufferVertex(tri->a, textureIndex, textureOffset, tri->c, tri->b);
      appendBufferVertex(tri->b, textureIndex, textureOffset, tri->a, tri->c);
      appendBufferVertex(tri->c, textureIndex, textureOffset, tri->b, tri->a);

    } else if (auto quad = primitive.ptr<RenderQuad>()) {
      tie(textureIndex, textureOffset) = addCurrentTexture(std::move(quad->texture));

      // = prev and next are altered - the diagonal across the quad is bad for the rounding check
      appendBufferVertex(quad->a, textureIndex, textureOffset, quad->d, quad->b);
      appendBufferVertex(quad->b, textureIndex, textureOffset, quad->a, quad->c); //
      appendBufferVertex(quad->c, textureIndex, textureOffset, quad->b, quad->d);

      appendBufferVertex(quad->a, textureIndex, textureOffset, quad->d, quad->b);
      appendBufferVertex(quad->c, textureIndex, textureOffset, quad->b, quad->d); //
      appendBufferVertex(quad->d, textureIndex, textureOffset, quad->c, quad->a);

    } else if (auto poly = primitive.ptr<RenderPoly>()) {
      if (poly->vertexes.size() > 2) {
        tie(textureIndex, textureOffset) = addCurrentTexture(std::move(poly->texture));

        for (size_t i = 1; i < poly->vertexes.size() - 1; ++i) {
            RenderVertex const& a = poly->vertexes[0],
                                b = poly->vertexes[i],
                                c = poly->vertexes[i + 1];
          appendBufferVertex(a, textureIndex, textureOffset, c, b);
          appendBufferVertex(b, textureIndex, textureOffset, a, c);
          appendBufferVertex(c, textureIndex, textureOffset, b, a);
        }
      }
    }
  }

  vertexBuffers.reserve(primitives.size() * 6);
  finishCurrentBuffer();

  for (auto const& vb : oldVertexBuffers)
    glDeleteBuffers(1, &vb.vertexBuffer);
}

bool OpenGlRenderer::logGlErrorSummary(String prefix) {
  if (GLenum error = glGetError()) {
    Logger::error("{}: ", prefix);
    do {
      if (error == GL_INVALID_ENUM) {
        Logger::error("GL_INVALID_ENUM");
      } else if (error == GL_INVALID_VALUE) {
        Logger::error("GL_INVALID_VALUE");
      } else if (error == GL_INVALID_OPERATION) {
        Logger::error("GL_INVALID_OPERATION");
      } else if (error == GL_INVALID_FRAMEBUFFER_OPERATION) {
        Logger::error("GL_INVALID_FRAMEBUFFER_OPERATION");
      } else if (error == GL_OUT_OF_MEMORY) {
        Logger::error("GL_OUT_OF_MEMORY");
#ifdef GL_STACK_UNDERFLOW
      } else if (error == GL_STACK_UNDERFLOW) {
        Logger::error("GL_STACK_UNDERFLOW");
#endif
#ifdef GL_STACK_OVERFLOW
      } else if (error == GL_STACK_OVERFLOW) {
        Logger::error("GL_STACK_OVERFLOW");
#endif
      } else {
        Logger::error("<UNRECOGNIZED GL ERROR>");
      }
    } while ((error = glGetError()));
    return true;
  }
  return false;
}

void OpenGlRenderer::uploadTextureImage(PixelFormat pixelFormat, Vec2U size, uint8_t const* data) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  Maybe<GLenum> internalFormat;
  GLenum format;
  GLenum type = GL_UNSIGNED_BYTE;
  if (pixelFormat == PixelFormat::RGB24)
    format = GL_RGB;
  else if (pixelFormat == PixelFormat::RGBA32)
    format = GL_RGBA;
  else if (pixelFormat == PixelFormat::BGR24)
    format = GL_BGR;
  else if (pixelFormat == PixelFormat::BGRA32)
    format = GL_BGRA;
  else {
    type = GL_FLOAT;
    if (pixelFormat == PixelFormat::RGB_F) {
      internalFormat = GL_RGB32F;
      format = GL_RGB;
    } else if (pixelFormat == PixelFormat::RGBA_F) {
      internalFormat = GL_RGBA32F;
      format = GL_RGBA;
    } else
      throw RendererException("Unsupported texture format in OpenGlRenderer::uploadTextureImage");
  }

  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat.value(format), size[0], size[1], 0, format, type, data);
}

void OpenGlRenderer::flushImmediatePrimitives(Mat3F const& transformation) {
  if (m_immediatePrimitives.empty())
    return;

  // Recording pass copies everything into the current segment while STILL
  // drawing normally below -- the recorded frame renders identically.
  if (m_recording && !m_recordingSegments.empty())
    m_recordingSegments.last().primitives.appendAll(m_immediatePrimitives);

  m_immediateRenderBuffer->set(m_immediatePrimitives);
  m_immediatePrimitives.resize(0);
  renderGlBuffer(*m_immediateRenderBuffer, transformation);
}

bool OpenGlRenderer::beginPrimitiveRecording() {
  if (m_recording)
    return false;
  flushImmediatePrimitives();
  m_recording = true;
  m_recordingSegments.clear();
  m_recordingSegments.append(RecordedSegment{m_scissorRect, {}});
  return true;
}

List<Renderer::RecordedSegment> OpenGlRenderer::endPrimitiveRecording() {
  if (!m_recording)
    return {};
  flushImmediatePrimitives();
  m_recording = false;
  // Scissor changes with no primitives under them (widget-tree walks set the
  // scissor for every widget, drawn or not) would replay as pointless
  // flush-inducing state changes; drop them.
  filter(m_recordingSegments, [](RecordedSegment const& segment) {
      return !segment.primitives.empty();
    });
  return std::move(m_recordingSegments);
}

void OpenGlRenderer::playPrimitiveRecording(List<RecordedSegment> const& recording) {
  for (auto const& segment : recording) {
    // setScissorRect flushes pending primitives itself when the scissor
    // actually changes, preserving the recorded draw grouping/order.
    setScissorRect(segment.scissor);
    m_immediatePrimitives.appendAll(segment.primitives);
  }
}

auto OpenGlRenderer::createGlTexture(ImageView const& image, TextureAddressing addressing, TextureFiltering filtering)
    ->RefPtr<GlLoneTexture> {
  auto glLoneTexture = make_ref<GlLoneTexture>();
  glLoneTexture->textureFiltering = filtering;
  glLoneTexture->textureAddressing = addressing;
  glLoneTexture->textureSize = image.size;

  glGenTextures(1, &glLoneTexture->textureId);
  if (glLoneTexture->textureId == 0)
    throw RendererException("Could not generate texture in OpenGlRenderer::createGlTexture");

  glBindTexture(GL_TEXTURE_2D, glLoneTexture->textureId);

  if (addressing == TextureAddressing::Clamp) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  glLoneTexture->textureFiltering = effectiveTextureFiltering(image.format, filtering);
  setTextureFiltering(glLoneTexture->textureFiltering);


  if (!image.empty())
    uploadTextureImage(image.format, image.size, image.data);

  return glLoneTexture;
}

auto OpenGlRenderer::createGlRenderBuffer() -> shared_ptr<GlRenderBuffer> {
  auto glrb = make_shared<GlRenderBuffer>();
  glrb->whiteTexture = m_whiteTexture;
  glrb->useMultiTexturing = m_useMultiTexturing;
  return glrb;
}

void OpenGlRenderer::renderGlBuffer(GlRenderBuffer const& renderBuffer, Mat3F const& transformation) {
  for (auto const& vb : renderBuffer.vertexBuffers) {
    glUniformMatrix3fv(m_vertexTransformUniform, 1, GL_TRUE, transformation.ptr());

    if (m_currentEffect->includeVBTextures) {
      for (size_t i = 0; i < vb.textures.size(); ++i) {
        glUniform2f(m_textureSizeUniforms[i], vb.textures[i].size[0], vb.textures[i].size[1]);
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, vb.textures[i].texture);
      }
    }

    for (auto const& p : m_currentEffect->textures) {
      if (p.second.textureValue) {
        glActiveTexture(GL_TEXTURE0 + p.second.textureUnit);
        glBindTexture(GL_TEXTURE_2D, p.second.textureValue->textureId);
      }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vb.vertexBuffer);

    glEnableVertexAttribArray(m_positionAttribute);
    glEnableVertexAttribArray(m_texCoordAttribute);
    glEnableVertexAttribArray(m_colorAttribute);
    glEnableVertexAttribArray(m_dataAttribute);

    glVertexAttribPointer(m_positionAttribute, 2, GL_FLOAT, GL_FALSE, sizeof(GlRenderVertex), (GLvoid*)offsetof(GlRenderVertex, pos));
    glVertexAttribPointer(m_texCoordAttribute, 2, GL_FLOAT, GL_FALSE, sizeof(GlRenderVertex), (GLvoid*)offsetof(GlRenderVertex, uv));
    glVertexAttribPointer(m_colorAttribute, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlRenderVertex), (GLvoid*)offsetof(GlRenderVertex, color));
    glVertexAttribIPointer(m_dataAttribute, 1, GL_INT, sizeof(GlRenderVertex), (GLvoid*)offsetof(GlRenderVertex, pack));

    glDrawArrays(GL_TRIANGLES, 0, vb.vertexCount);
  }
}

//Assumes the passed effect program is currently in use.
void OpenGlRenderer::setupGlUniforms(Effect& effect, Vec2U screenSize) {
  m_positionAttribute = effect.getAttribute("vertexPosition");
  m_colorAttribute = effect.getAttribute("vertexColor");
  m_texCoordAttribute = effect.getAttribute("vertexTextureCoordinate");
  m_dataAttribute = effect.getAttribute("vertexData");

  m_textureUniforms.clear();
  m_textureSizeUniforms.clear();
  if (effect.includeVBTextures) {
    for (size_t i = 0; i < MultiTextureCount; ++i) {
      m_textureUniforms.append(effect.getUniform(strf("texture{}", i).c_str()));
      m_textureSizeUniforms.append(effect.getUniform(strf("textureSize{}", i).c_str()));
    }
  }
  m_screenSizeUniform = effect.getUniform("screenSize");
  m_vertexTransformUniform = effect.getUniform("vertexTransform");

  if (effect.includeVBTextures) {
    for (size_t i = 0; i < MultiTextureCount; ++i)
      glUniform1i(m_textureUniforms[i], i);
  }

  glUniform2f(m_screenSizeUniform, screenSize[0], screenSize[1]);
  
  for (auto& param : effect.scriptables) {
    auto ptr = &param.second;
    auto mvalue = ptr->parameterValue;
    if (mvalue) {
      RenderEffectParameter value = mvalue.value();
      if (auto v = value.ptr<bool>())
        glUniform1i(ptr->parameterUniform, *v);
      else if (auto v = value.ptr<int>())
        glUniform1i(ptr->parameterUniform, *v);
      else if (auto v = value.ptr<float>())
        glUniform1f(ptr->parameterUniform, *v);
      else if (auto v = value.ptr<Vec2F>())
        glUniform2f(ptr->parameterUniform, (*v)[0], (*v)[1]);
      else if (auto v = value.ptr<Vec3F>())
        glUniform3f(ptr->parameterUniform, (*v)[0], (*v)[1], (*v)[2]);
      else if (auto v = value.ptr<Vec4F>())
        glUniform4f(ptr->parameterUniform, (*v)[0], (*v)[1], (*v)[2], (*v)[3]);
    }
  }
}

RefPtr<OpenGlRenderer::GlFrameBuffer> OpenGlRenderer::getGlFrameBuffer(String const& id) {
  if (auto ptr = m_frameBuffers.ptr(id))
    return *ptr;
  else
    throw RendererException::format("Frame buffer '{}' does not exist", id);
}

void OpenGlRenderer::blitGlFrameBuffer(RefPtr<GlFrameBuffer> const& frameBuffer) {
#ifdef STAR_SYSTEM_SWITCH
  static int64_t s_blitRan = 0, s_blitSkipped = 0;
  if (frameBuffer->blitted)
    ++s_blitSkipped;
  else
    ++s_blitRan;
  if ((s_blitRan + s_blitSkipped) % 300 == 0)
    Logger::info("[blit] ran={} skipped={} draw-fbo-target={} screenViewport={}x{} offset={},{}",
        s_blitRan, s_blitSkipped, m_screenFbo, m_screenViewportSize[0], m_screenViewportSize[1], m_screenOffset[0], m_screenOffset[1]);
#endif
  if (frameBuffer->blitted)
    return;

  auto& viewport = m_screenViewportSize;
  auto& off  = m_screenOffset;
  // Read region is the framebuffer's ACTUAL content size (which may be downscaled
  // by sizeDiv/sizeMul), and we blit it stretched to the full screen viewport. This
  // both fixes a latent bug (the source rect previously assumed full screen size)
  // and implements the upscale for render-scaled framebuffers.
  Vec2U src = framebufferTextureSize(m_screenSize, frameBuffer->sizeDiv, frameBuffer->sizeMul);
  bool scaled = (frameBuffer->sizeDiv != 1) || (frameBuffer->sizeMul != 1.0f);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_screenFbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, frameBuffer->id);
  glBlitFramebuffer(
    0, 0, src[0], src[1],
    off[0], off[1], off[0] + viewport[0], off[1] + viewport[1],
    GL_COLOR_BUFFER_BIT, scaled ? GL_LINEAR : GL_NEAREST
  );

  frameBuffer->blitted = true;
}

void OpenGlRenderer::switchGlFrameBuffer(RefPtr<GlFrameBuffer> const& frameBuffer) {
  if (m_currentFrameBuffer == frameBuffer)
    return;

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer->id);
  // Premultiplied overlay targets accumulate premultiplied color plus correct
  // coverage in alpha; everything else uses the standard blend.
  if (frameBuffer->premultiplied)
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  else
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  m_currentFrameBuffer = frameBuffer;
}

GLuint OpenGlRenderer::Effect::getAttribute(String const& name) {
  auto find = attributes.find(name);
  if (find == attributes.end()) {
    GLuint attrib = glGetAttribLocation(program, name.utf8Ptr());
    attributes[name] = attrib;
    return attrib;
  }
  return find->second;
}

GLuint OpenGlRenderer::Effect::getUniform(String const& name) {
  auto find = uniforms.find(name);
  if (find == uniforms.end()) {
    GLuint uniform = glGetUniformLocation(program, name.utf8Ptr());
    uniforms[name] = uniform;
    return uniform;
  }
  return find->second;
}


}
