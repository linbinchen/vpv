#include <fstream>
#include <cmath>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Shader.hpp>
#include <SFML/Window/Event.hpp>
#include <SFML/OpenGL.hpp>

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

extern "C" {
#include "iio.h"
}

#include "globals.hpp"
#include "Window.hpp"
#include "Sequence.hpp"
#include "View.hpp"
#include "Player.hpp"
#include "Colormap.hpp"
#include "Image.hpp"
#include "Shader.hpp"
#include "layout.hpp"

static ImRect getClipRect();

static bool file_exists(const char *fileName)
{
    std::ifstream infile(fileName);
    return infile.good();
}

Window::Window()
{
    static int id = 0;
    id++;
    ID = "Window " + std::to_string(id);

    opened = true;
    index = 0;
}

void Window::display()
{
    screenshot = false;

    int index = std::find(gWindows.begin(), gWindows.end(), this) - gWindows.begin();
    bool isKeyFocused = !ImGui::GetIO().WantCaptureKeyboard && index <= 9
        && ImGui::IsKeyPressed(sf::Keyboard::Num1 + index) && !ImGui::IsKeyDown(sf::Keyboard::LAlt);

    if (isKeyFocused && !opened) {
        opened = true;
        relayout(false);
    }

    if (isKeyFocused) {
        ImGui::SetNextWindowFocus();
    }

    if (!opened) {
        return;
    }

    if (forceGeometry) {
        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSize(size);
        forceGeometry = false;
    }

    auto prevcolor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    char buf[512];
    snprintf(buf, sizeof(buf), "%s###%s", getTitle().c_str(), ID.c_str());
    if (!ImGui::Begin(buf, &opened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = prevcolor;
        return;
    }

    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = prevcolor;

    // just closed
    if (!opened) {
        relayout(false);
    }

    if (ImGui::IsWindowFocused()) {
        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::Space)) {
            this->index = (this->index + 1) % sequences.size();
        }
        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::BackSpace)) {
            this->index = (sequences.size() + this->index - 1) % sequences.size();
        }
    }

    Sequence* seq = getCurrentSequence();

    if (!seq) {
        ImGui::End();
        return;
    }

    if (!seq->valid || !seq->player) {
        ImGui::End();
        return;
    }

    displaySequence(*seq);

    ImGui::End();
}

void Window::displaySequence(Sequence& seq)
{
    Texture& texture = seq.texture;
    View* view = seq.view;

    bool focusedit = false;

    // display the image
    if (seq.colormap && seq.view && seq.player) {
        ImRect clip = getClipRect();

        ImVec2 p1 = view->window2image(ImVec2(0, 0), texture.size, clip.Max - clip.Min);
        ImVec2 p2 = view->window2image(clip.Max - clip.Min, texture.size, clip.Max - clip.Min);
        seq.requestTextureArea(ImRect(p1, p2));

        ImVec2 TL = view->image2window(ImVec2(0, 0), texture.size, clip.Max - clip.Min);
        ImVec2 BR = view->image2window(texture.size, texture.size, clip.Max - clip.Min);

        TL += clip.Min;
        BR += clip.Min;

        ImGui::PushClipRect(clip.Min, clip.Max, true);
        ImGui::GetWindowDrawList()->CmdBuffer.back().shader = &seq.colormap->shader->shader;
        ImGui::GetWindowDrawList()->CmdBuffer.back().scale = seq.colormap->getScale();
        ImGui::GetWindowDrawList()->CmdBuffer.back().bias = seq.colormap->getBias();
        ImGui::GetWindowDrawList()->AddImage((void*)(size_t)texture.id, TL, BR);
        ImGui::PopClipRect();

        contentRect = clip;
    }

    if (ImGui::IsWindowFocused()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;

        bool showingTooltip = ImGui::IsMouseDown(2) || (!ImGui::GetIO().WantCaptureKeyboard &&
                                                        ImGui::IsKeyDown(sf::Keyboard::T));
        bool zooming = !ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyDown(sf::Keyboard::Z);

        if (showingTooltip) {
            displayTooltip(seq);
        }

        if (zooming && ImGui::GetIO().MouseWheel != 0.f) {
            ImRect clip = getClipRect();
            ImVec2 cursor = ImGui::GetMousePos() - clip.Min;
            ImVec2 pos = view->window2image(cursor, texture.size, clip.Max - clip.Min);

            view->changeZoom(view->zoom * (1 + 0.1 * ImGui::GetIO().MouseWheel));

            ImVec2 pos2 = view->window2image(cursor, texture.size, clip.Max - clip.Min);
            view->center += (pos - pos2) / texture.size;
        }
        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::I)) {
            view->changeZoom(std::pow(2, floor(log2(view->zoom) + 1)));
        }
        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::O)) {
            view->changeZoom(std::pow(2, ceil(log2(view->zoom) - 1)));
        }

        if (!ImGui::IsMouseClicked(0) && ImGui::IsMouseDown(0) && (delta.x || delta.y)) {
            ImRect clip = getClipRect();
            ImVec2 pos = view->window2image(ImVec2(0, 0), texture.size, clip.Max - clip.Min);
            ImVec2 pos2 = view->window2image(delta, texture.size, clip.Max - clip.Min);
            ImVec2 diff = pos - pos2;
            view->center += diff / texture.size;
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::R)) {
            view->center = ImVec2(0.5, 0.5);
            if (ImGui::IsKeyDown(sf::Keyboard::LShift)) {
                view->resetZoom();
            } else {
                view->setOptimalZoom(contentRect.GetSize(), texture.getSize());
            }
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && !zooming && ImGui::GetIO().MouseWheel) {
            const Image* img = seq.getCurrentImage();
            if (ImGui::IsKeyDown(sf::Keyboard::LShift) && img) {
                seq.colormap->radius = std::max(0.f, seq.colormap->radius * (1.f + .1f * ImGui::GetIO().MouseWheel));
            } else if (img) {
                float newcenter = seq.colormap->center + seq.colormap->radius * .1f * ImGui::GetIO().MouseWheel;
                seq.colormap->center = std::min(std::max(newcenter, img->min), img->max);
            }
            seq.colormap->print();
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyDown(sf::Keyboard::LShift) && (delta.x || delta.y)) {
            ImRect clip = getClipRect();
            ImVec2 cursor = ImGui::GetMousePos() - clip.Min;
            ImVec2 pos = view->window2image(cursor, texture.size, clip.Max - clip.Min);
            const Image* img = seq.getCurrentImage();
            if (img && pos.x >= 0 && pos.y >= 0 && pos.x < img->w && pos.y < img->h) {
                int nb = img->format;
                float v[nb];
                memset(v, 0, nb*sizeof(float));
                img->getPixelValueAt(pos.x, pos.y, v, nb);
                float mean = 0;
                for (int i = 0; i < nb; i++) mean += v[i] / nb;
                if (!std::isnan(mean) && !std::isinf(mean)) {
                    seq.colormap->center = mean;
                    seq.colormap->print();
                }
            }
        }

        if (seq.player) {
            seq.player->checkShortcuts();
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::A)) {
            if (ImGui::IsKeyDown(sf::Keyboard::LShift)) {
                const Image* img = seq.getCurrentImage();
                if (img->min > -1e-3 && img->max < 1.f+1e-3) {
                    seq.colormap->center = 0.5f;
                    seq.colormap->radius = 0.5f;
                } else {
                    seq.colormap->center = 127.5f;
                    seq.colormap->radius = 127.5f;
                }
            } else if (ImGui::IsKeyDown(sf::Keyboard::LControl)) {
                ImRect clip = getClipRect();
                ImVec2 p1 = view->window2image(ImVec2(0, 0), texture.size, clip.Max - clip.Min);
                ImVec2 p2 = view->window2image(clip.Max - clip.Min, texture.size, clip.Max - clip.Min);
                seq.smartAutoScaleAndBias(p1, p2);
            } else {
                seq.autoScaleAndBias();
            }
            seq.colormap->print();
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::S)) {
            if (ImGui::IsKeyDown(sf::Keyboard::LShift)) {
                seq.colormap->previousShader();
            } else {
                seq.colormap->nextShader();
            }
            seq.colormap->print();
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::E)) {
            if (!*seq.editprog) {
                int id = 0;
                while (gSequences[id] != &seq && id < gSequences.size())
                    id++;
                sprintf(seq.editprog, "%d", id);
                seq.edittype = ImGui::IsKeyDown(sf::Keyboard::LShift)
                                ? Sequence::EditType::GMIC : Sequence::EditType::PLAMBDA;
            }
            focusedit = true;
        }

        if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(sf::Keyboard::Comma)) {
            screenshot = true;
        }
    }

    if (seq.editprog[0]) {
        if (focusedit)
            ImGui::SetKeyboardFocusHere();
        const char* name = seq.edittype == Sequence::EditType::PLAMBDA ? "plambda" : "gmic";
        if (ImGui::InputText(name, seq.editprog, sizeof(seq.editprog),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            seq.force_reupload = true;
        }
        if (!seq.editprog[0]) {
            seq.force_reupload = true;
        }
    }
}

void Window::displayTooltip(Sequence& seq)
{
    Texture& texture = seq.texture;
    View* view = seq.view;

    ImRect winclip = getClipRect();
    ImVec2 cursor = ImGui::GetMousePos() - winclip.Min;
    ImVec2 im = view->window2image(cursor, texture.size, winclip.Max - winclip.Min);
    im.x = std::floor(im.x);
    im.y = std::floor(im.y);

    ImGui::BeginTooltip();
    {
        ImGui::Text("(%d, %d)", (int)im.x, (int)im.y);

        const Image* img = seq.getCurrentImage();
        if (img && im.x >= 0 && im.y >= 0 && im.x < img->w && im.y < img->h) {
            float v[4] = {0};
            img->getPixelValueAt(im.x, im.y, v, 4);
            if (img->format == Image::R) {
                ImGui::Text("gray (%g)", v[0]);
            } else if (img->format == Image::RG) {
                ImGui::Text("flow (%g, %g)", v[0], v[1]);
            } else if (img->format == Image::RGB) {
                ImGui::Text("rgb (%g, %g, %g)", v[0], v[1], v[2]);
            } else if (img->format == Image::RGBA) {
                ImGui::Text("rgba (%g, %g, %g, %g)", v[0], v[1], v[2], v[3]);
            }
        }
    }
    ImGui::EndTooltip();
}

void Window::displaySettings()
{
    if (ImGui::Checkbox("Opened", &opened))
        relayout(false);
    ImGui::Text("Sequences");
    ImGui::SameLine(); ImGui::ShowHelpMarker("Choose which sequences are associated with this window");
    ImGui::BeginChild("scrolling", ImVec2(350, ImGui::GetItemsLineHeightWithSpacing()*3 + 20),
                      true, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto seq : gSequences) {
        auto it = std::find(sequences.begin(), sequences.end(), seq);
        bool selected = it != sequences.end();
        ImGui::PushID(seq);
        if (ImGui::Selectable(seq->glob.c_str(), selected)) {
            if (!selected) {
                sequences.push_back(seq);
            } else {
                sequences.erase(it);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SliderInt("Index", &index, 0, sequences.size()-1);
    ImGui::SameLine(); ImGui::ShowHelpMarker("Choose which sequence to display in the window (space / backspace)");
    index = (index + sequences.size()) % sequences.size();
}

void Window::postRender()
{
    if (!screenshot) return;

    ImVec2 winSize = ImGui::GetIO().DisplaySize;
    int x = contentRect.Min.x;
    int y = winSize.y - contentRect.Max.y - 0;
    int w = contentRect.Max.x - contentRect.Min.x;
    int h = contentRect.Max.y - contentRect.Min.y + 0;
    int size = 3 * w * h;

    float* data = new float[size];
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(x, y, w, h, GL_RGB, GL_FLOAT, data);
    for (int i = 0; i < size; i++)
        data[i] *= 255.f;
    for (int y = 0; y < h/2; y++) {
        for (int i = 0; i < 3*w; i++) {
            float v = data[(h - y - 1)*(3*w) + i];
            data[(h - y - 1)*(3*w) + i] = data[y*(3*w) + i];
            data[y*(3*w) + i] = v;
        }
    }

    const char* filename_fmt = getenv("SCREENSHOT");
    if (!filename_fmt) filename_fmt = "screenshot_%d.png";
    int i = 1;
    while (true) {
        char filename[512];
        snprintf(filename, sizeof(filename), filename_fmt, i);
        if (!file_exists(filename)) {
            iio_save_image_float_vec(filename, data, w, h, 3);
            printf("Screenshot saved to '%s'.\n", filename);
            break;
        }
        i++;
    }
    delete[] data;
}

Sequence* Window::getCurrentSequence() const
{
    if (sequences.empty())
        return nullptr;
    return sequences[index];
}

std::string Window::getTitle() const
{
    const Sequence* seq = getCurrentSequence();
    if (!seq)
        return "(no sequence associated)";
    return seq->getTitle();
}

ImRect getRenderingRect(ImVec2 texSize, ImRect* windowRect)
{
    ImVec2 pos = ImGui::GetWindowPos() + ImGui::GetCursorPos();
    ImVec2 pos2 = pos + ImGui::GetContentRegionAvail();
    if (windowRect) {
        *windowRect = ImRect(pos, pos2);
    }

    ImVec2 diff = pos2 - pos;
    float aspect = (float) texSize.x / texSize.y;
    float nw = std::max(diff.x, diff.y * aspect);
    float nh = std::max(diff.y, diff.x / aspect);
    ImVec2 offset = ImVec2(nw - diff.x, nh - diff.y);

    pos -= offset / 2;
    pos2 += offset / 2;
    return ImRect(pos, pos2);
}

ImRect getClipRect()
{
    ImVec2 pos = ImGui::GetWindowPos() + ImGui::GetCursorPos();
    ImVec2 pos2 = pos + ImGui::GetContentRegionAvail();
    return ImRect(pos, pos2);
}

