#include "render_gl/GlProgram.hpp"

#include "core/Log.hpp"

#include <format>
#include <stdexcept>
#include <string>

namespace chroma::gl {
namespace {

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

Program::Program(const Context& context, std::string_view name, const char* vertexSource, const char* fragmentSource)
    : api_(&context.api()), lifetime_(context.lifetime()) {
    const GLuint vertex = compile(kVertexShader, vertexSource, name);
    GLuint fragment = 0;
    try {
        fragment = compile(kFragmentShader, fragmentSource, name);
    } catch (...) {
        api_->deleteShader(vertex);
        throw;
    }
    program_ = api_->createProgram();
    api_->attachShader(program_, vertex);
    api_->attachShader(program_, fragment);
    api_->linkProgram(program_);
    api_->deleteShader(vertex);  // flagged for deletion; freed together with the program
    api_->deleteShader(fragment);

    GLint linked = 0;
    api_->getProgramiv(program_, kLinkStatus, &linked);
    GLint length = 0;
    api_->getProgramiv(program_, kInfoLogLength, &length);
    std::string log(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) api_->getProgramInfoLog(program_, length, nullptr, log.data());
    log = trimmed(std::move(log));
    if (!linked) {
        api_->deleteProgram(program_);
        program_ = 0;
        throw std::runtime_error(std::format("GLSL program '{}' failed to link: {}", name, log));
    }
    if (!log.empty()) logf("GLSL program '{}' link log: {}", name, log);
}

Program::~Program() {
    if (program_ && !lifetime_.expired()) api_->deleteProgram(program_);
}

GLuint Program::compile(GLenum stage, const char* source, std::string_view name) const {
    const GLuint shader = api_->createShader(stage);
    api_->shaderSource(shader, 1, &source, nullptr);
    api_->compileShader(shader);
    GLint compiled = 0;
    api_->getShaderiv(shader, kCompileStatus, &compiled);
    GLint length = 0;
    api_->getShaderiv(shader, kInfoLogLength, &length);
    std::string log(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) api_->getShaderInfoLog(shader, length, nullptr, log.data());
    log = trimmed(std::move(log));
    const char* stageName = stage == kVertexShader ? "vertex" : "fragment";
    if (!compiled) {
        api_->deleteShader(shader);
        throw std::runtime_error(std::format("GLSL {} stage of '{}' failed to compile: {}", stageName, name, log));
    }
    if (!log.empty()) logf("GLSL {} stage of '{}': {}", stageName, name, log);
    return shader;
}

}  // namespace chroma::gl
