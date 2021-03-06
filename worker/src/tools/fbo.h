#pragma once

#include "gl.h"

class Fbo {
public:
    Fbo();
    Fbo(const unsigned int &_width, const unsigned int &_height, bool _depth = true);
    virtual ~Fbo();

    const unsigned int getWidth() const { return m_width; };
    const unsigned int getHeight() const { return m_height; };
    const GLuint getGlHandle() const { return m_id; }
    const GLuint getTextureId() const { return m_texture; };

    void resize(const unsigned int &_width, const unsigned int &_height, bool _depth = true);

    void bind();
    void unbind();

protected:
    GLuint  m_id;
    GLuint  m_old_fbo_id;

    GLuint  m_texture;
    GLuint  m_depth_buffer;

    unsigned int m_width;
    unsigned int m_height;

    bool    m_allocated;
    bool    m_binded;
};