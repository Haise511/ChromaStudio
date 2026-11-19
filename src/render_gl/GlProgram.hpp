#pragma once
// ChromaStudio Engine · render_gl · linked GLSL program (one vertex + one fragment stage).

#include "render_gl/GlContext.hpp"

#include <memory>
#include <string_view>

namespace chroma::gl {

class Program {
public:
    /// Compiles and links; throws std::runtime_error with the driver log on failure.
    Program(const Context& context, std::string_view name, const char* vertexSource, const char* fragmentSource);
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    void use() const { api_->useProgram(program_); }
    /// -1 for an unknown or inactive uniform (writes to -1 are ignored by GL).
    [[nodiscard]] GLint uniform(const char* name) const { return api_->getUniformLocation(program_, name); }
    [[nodiscard]] GLuint id() const noexcept { return program_; }

private:
    [[nodiscard]] GLuint compile(GLenum stage, const char* source, std::string_view name) const;

    const Api* api_;
    std::weak_ptr<const void> lifetime_;
    GLuint program_ = 0;
};

}  // namespace chroma::gl
