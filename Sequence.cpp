#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <glob.h>
#include <algorithm>

#include <SFML/OpenGL.hpp>

extern "C" {
#include "iio.h"
}

#include "Sequence.hpp"
#include "Player.hpp"
#include "View.hpp"
#include "alphanum.hpp"

Texture::~Texture() {
    glDeleteTextures(1, &id);
}

const char* getGLError(GLenum error)
{
#define casereturn(x) case x: return #x
    switch (error) {
        casereturn(GL_INVALID_ENUM);
        casereturn(GL_INVALID_VALUE);
        casereturn(GL_INVALID_OPERATION);
        casereturn(GL_INVALID_FRAMEBUFFER_OPERATION);
        casereturn(GL_OUT_OF_MEMORY);
        default:
        case GL_NO_ERROR:
        return "";
    }
#undef casereturn
}

#define GLDEBUG(x) \
    x; \
{ \
    GLenum e; \
    while((e = glGetError()) != GL_NO_ERROR) \
    { \
        printf("%s:%s:%d for call %s", getGLError(e), __FILE__, __LINE__, #x); \
        exit(1); \
    } \
}

void Texture::create(int w, int h, uint type, uint format)
{
    if (id == -1) {
        glGenTextures(1, &id);
    }

    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, w, h, 0, format, type, NULL);
    GLDEBUG();

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    GLDEBUG();

    glBindTexture(GL_TEXTURE_2D, 0);

    size.x = w;
    size.y = h;
    type = type;
    format = format;
}

Sequence::Sequence()
{
    static int id = 0;
    id++;
    ID = "Sequence " + std::to_string(id);

    view = nullptr;
    player = nullptr;
    shader = "default";
    scale = 1.0f;
    bias = 0.0f;

    valid = false;
    visible = false;

    loadedFrame = -1;
    loadedRect = ImRect();

    glob.reserve(1024);
    glob_.reserve(1024);
    glob = "";
    glob_ = "";
}

void Sequence::loadFilenames() {
    glob_t res;
    ::glob(glob.c_str(), GLOB_TILDE | GLOB_NOSORT, NULL, &res);
    filenames.resize(res.gl_pathc);
    for(unsigned int j = 0; j < res.gl_pathc; j++) {
        filenames[j] = res.gl_pathv[j];
    }
    globfree(&res);
    std::sort(filenames.begin(), filenames.end(), doj::alphanum_less<std::string>());

    valid = filenames.size() > 0;
    strcpy(&glob_[0], &glob[0]);

    loadedFrame = -1;
}

void Sequence::loadFrame(int frame)
{
    if (pixelCache.count(frame)) {
        return;
    }

    const std::string& filename = filenames[frame - 1];

    int w, h, d;
    float* pixels = iio_read_image_float_vec(filename.c_str(), &w, &h, &d);

    pixelCache[frame].w = w;
    pixelCache[frame].h = h;
    pixelCache[frame].type = Image::FLOAT;
    pixelCache[frame].format = (Image::Format) d;
    pixelCache[frame].pixels = pixels;
}

void Sequence::loadTextureIfNeeded()
{
    if (valid && visible && player) {
        int frame = player->frame;

        if (!pixelCache.count(frame)) {
            loadFrame(frame);
            if (!pixelCache.count(frame)) {
                return;
            }
        }

        if (loadedFrame != player->frame) {
            loadedRect = ImRect();
        }

        const Image& img = pixelCache[frame];
        int w = img.w;
        int h = img.h;
        bool reupload = false;

        ImVec2 u, v;
        view->compute(ImVec2(w, h), u, v);

        ImRect area(u.x*w, u.y*h, v.x*w+1, v.y*h+1);
        area.Floor();
        area.Clip(ImRect(0, 0, w, h));

        if (!loadedRect.ContainsInclusive(area)) {
            reupload = true;
        }

        if (reupload) {
            area.Expand(32);  // to avoid multiple uploads during zoom-out
            area.Clip(ImRect(0, 0, w, h));

            uint gltype;
            if (img.type == Image::UINT8)
                gltype = GL_UNSIGNED_BYTE;
            else if (img.type == Image::FLOAT)
                gltype = GL_FLOAT;
            else
                assert(0);

            uint glformat;
            if (img.format == Image::R)
                glformat = GL_RED;
            else if (img.format == Image::RG)
                glformat = GL_RG;
            else if (img.format == Image::RGB)
                glformat = GL_RGB;
            else if (img.format == Image::RGBA)
                glformat = GL_RGBA;
            else
                assert(0);

            if (texture.size.x != w || texture.size.y != h
                || texture.type != gltype || texture.format != glformat) {
                texture.create(w, h, gltype, glformat);
            }

            size_t elsize;
            if (img.type == Image::UINT8) elsize = sizeof(uint8_t);
            else if (img.type == Image::FLOAT) elsize = sizeof(float);
            const uint8_t* data = (uint8_t*) img.pixels + elsize*(w * (int)area.Min.y + (int)area.Min.x)*img.format;

            glBindTexture(GL_TEXTURE_2D, texture.id);
            GLDEBUG();

            glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
            GLDEBUG();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, area.GetWidth(), area.GetHeight(), 0,
                            glformat, gltype, img.pixels);
            GLDEBUG();
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            GLDEBUG();

            loadedFrame = frame;
            loadedRect.Add(area);
        }
    }
}

void Sequence::autoScaleAndBias()
{
    scale = 1.f;
    bias = 0.f;

    if (valid && visible && player) {
        int frame = player->frame;

        if (!pixelCache.count(frame)) {
            return;
        }

        const Image& img = pixelCache[frame];
        if (img.type != Image::FLOAT) {
            return;
        }

        float* pixels = (float*) img.pixels;
        float min = FLT_MAX;
        float max = FLT_MIN;
        for (int i = 0; i < img.w*img.h*img.format; i++) {
            min = fminf(min, pixels[i]);
            max = fmaxf(max, pixels[i]);
        }

        float a = 1.f;
        float b = 0.f;
        if (fabsf(min - 0.f) < 0.01f && fabsf(max - 1.f) < 0.01f) {
            a = 1.f;
        } else {
            a = 1.f / (max - min);
            b = - min;
        }

        scale = a;
        bias = a*b;
    }
}
