// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fplbase/utilities.h"
#include "flatui/flatui.h"
#include "flatui/internal/micro_edit.h"
#include "SDL.h"

namespace fpl {
namespace gui {

Direction GetDirection(Layout layout) {
  return static_cast<Direction>(layout & ~(kDirHorizontal - 1));
}

Alignment GetAlignment(Layout layout) {
  return static_cast<Alignment>(layout & (kDirHorizontal - 1));
}

static const char *kDummyId = "__null_id__";
static const float kScrollSpeedDragDefault = 2.0f;
static const float kScrollSpeedWheelgDefault = 16.0f;
static const int32_t kDragStartThresholdDefault = 8;
static const int32_t kPointerIndexInvalid = -1;
static const vec2i kDragStartPoisitionInvalid = vec2i(-1, -1);

// This holds the transient state of a group while its layout is being
// calculated / rendered.
class Group {
 public:
  Group(Direction _direction, Alignment _align, int _spacing,
        size_t _element_idx)
      : direction_(_direction),
        align_(_align),
        spacing_(_spacing),
        size_(mathfu::kZeros2i),
        position_(mathfu::kZeros2i),
        element_idx_(_element_idx),
        margin_(mathfu::kZeros4i) {}

  // Extend this group with the size of a new element, and possibly spacing
  // if it wasn't the first element.
  void Extend(const vec2i &extension) {
    switch (direction_) {
      case kDirHorizontal:
        size_ = vec2i(size_.x() + extension.x() + (size_.x() ? spacing_ : 0),
                      std::max(size_.y(), extension.y()));
        break;
      case kDirVertical:
        size_ = vec2i(std::max(size_.x(), extension.x()),
                      size_.y() + extension.y() + (size_.y() ? spacing_ : 0));
        break;
      case kDirOverlay:
        size_ = vec2i(std::max(size_.x(), extension.x()),
                      std::max(size_.y(), extension.y()));
        break;
    }
  }

  Direction direction_;
  Alignment align_;
  int spacing_;
  vec2i size_;
  vec2i position_;
  size_t element_idx_;
  vec4i margin_;
};

// This holds transient state used while a GUI is being laid out / rendered.
// It is intentionally hidden from the interface.
// It is implemented as a singleton that the GUI element functions can access.

class InternalState;
InternalState *state = nullptr;

class InternalState : public Group {
 public:
  // We create one of these per GUI element, so new fields should only be
  // added when absolutely necessary.
  struct Element {
    Element(const vec2i &_size, const char *_id)
        : size(_size),
          extra_size(mathfu::kZeros2i),
          id(_id),
          interactive(false) {}
    vec2i size;        // Minimum on-screen size computed by layout pass.
    vec2i extra_size;  // Additional size in a scrolling area (TODO: remove?)
    const char *id;    // Id specified by the user.
    bool interactive;  // Wants to respond to user input.
  };

  InternalState(AssetManager &assetman, FontManager &fontman,
                InputSystem &input)
      : Group(kDirVertical, kAlignLeft, 0, 0),
        layout_pass_(true),
        virtual_resolution_(FLATUI_DEFAULT_VIRTUAL_RESOLUTION),
        matman_(assetman),
        renderer_(assetman.renderer()),
        input_(input),
        fontman_(fontman),
        clip_position_(mathfu::kZeros2i),
        clip_size_(mathfu::kZeros2i),
        clip_inside_(false),
        pointer_max_active_index_(kPointerIndexInvalid),
        gamepad_has_focus_element(false),
        gamepad_event(kEventHover) {
    SetScale();

    // Cache the state of multiple pointers, so we have to do less work per
    // interactive element.
    pointer_max_active_index_ = 0;  // Mouse is always active.
    // TODO: pointer_max_active_index_ should start at kPointerIndexInvalid
    // if on a touchscreen.
    for (int i = 0; i < InputSystem::kMaxSimultanuousPointers; i++) {
      pointer_buttons_[i] = &input.GetPointerButton(i);
      clip_mouse_inside_[i] = true;
      if (pointer_buttons_[i]->is_down() || pointer_buttons_[i]->went_down() ||
          pointer_buttons_[i]->went_up()) {
        pointer_max_active_index_ = std::max(pointer_max_active_index_, i);
      }
    }

    // If this assert hits, you likely are trying to created nested GUIs.
    assert(!state);

    state = this;

    // Load shaders ahead.
    image_shader_ = matman_.LoadShader("shaders/textured");
    assert(image_shader_);
    font_shader_ = matman_.LoadShader("shaders/font");
    assert(font_shader_);
    font_clipping_shader_ = matman_.LoadShader("shaders/font_clipping");
    assert(font_clipping_shader_);
    color_shader_ = matman_.LoadShader("shaders/color");
    assert(color_shader_);

    text_color_ = mathfu::kOnes4f;

    scroll_speed_drag_ = kScrollSpeedDragDefault;
    scroll_speed_wheel_ = kScrollSpeedWheelgDefault;
    drag_start_threshold_ =
        vec2i(kDragStartThresholdDefault, kDragStartThresholdDefault);
    current_pointer_ = kPointerIndexInvalid;

    fontman_.StartLayoutPass();
  }

  ~InternalState() { state = nullptr; }

  bool EqualId(const char *id1, const char *id2) {
    // We can do pointer compare, because we receive these ids from the user
    // and then store them.
    // We require the user to provide storage for the id as long as the GUI
    // is active, which guarantees pointer identity.
    // TODO: we either need to provide a way to clear persistent ids once a
    // GUI goes away entirely, or tell users not ever pass a c_str().
    // Better yet, replace this by hashes, as that makes generating ids in
    // loops easier. Bit expensive though.
    return id1 == id2;
  }

  template <int D>
  mathfu::Vector<int, D> VirtualToPhysical(const mathfu::Vector<float, D> &v) {
    return mathfu::Vector<int, D>(v * pixel_scale_ + 0.5f);
  }

  // Initialize the scaling factor for the virtual resolution.
  void SetScale() {
    auto scale = vec2(renderer_.window_size()) / virtual_resolution_;
    pixel_scale_ = std::min(scale.x(), scale.y());
  }

  // Retrieve the scaling factor for the virtual resolution.
  float GetScale() { return pixel_scale_; }

  // Set up an ortho camera for all 2D elements, with (0, 0) in the top left,
  // and the bottom right the windows size in pixels.
  // This is currently hardcoded to use overlay on top of the entire GL window.
  // If that ever changes, we also need to change our use of glScissor below.
  void SetOrtho() {
    auto res = renderer_.window_size();
    auto ortho_mat = mathfu::OrthoHelper<float>(
        0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()), 0.0f,
        -1.0f, 1.0f);
    renderer_.model_view_projection() = ortho_mat;
  }

  // Compute a space offset for a particular alignment for just the x or y
  // dimension.
  static vec2i AlignDimension(Alignment align, int dim, const vec2i &space) {
    vec2i dest(0, 0);
    switch (align) {
      case kAlignTop:  // Same as kAlignLeft.
        break;
      case kAlignCenter:
        dest[dim] += space[dim] / 2;
        break;
      case kAlignBottom:  // Same as kAlignRight.
        dest[dim] += space[dim];
        break;
    }
    return dest;
  }

  // Determines placement for the UI as a whole inside the available space
  // (screen).
  void PositionUI(float virtual_resolution, Alignment horizontal,
                  Alignment vertical) {
    if (layout_pass_) {
      virtual_resolution_ = virtual_resolution;
      SetScale();
    } else {
      auto space = renderer_.window_size() - size_;
      position_ += AlignDimension(horizontal, 0, space) +
                   AlignDimension(vertical, 1, space);
    }
  }

  // Switch from the layout pass to the render pass.
  void StartRenderPass() {
    // If you hit this assert, you are missing an EndGroup().
    assert(!group_stack_.size());

    // Do nothing if there is no elements.
    if (elements_.size() == 0) return;

    // Put in a sentinel element. We'll use this element to point to
    // when a group didn't exist during layout but it does during rendering.
    NewElement(mathfu::kZeros2i, "__sentinel__");

    // Update font manager if they need to upload font atlas texture.
    fontman_.StartRenderPass();

    position_ = mathfu::kZeros2i;
    size_ = elements_[0].size;

    layout_pass_ = false;
    element_it_ = elements_.begin();

    CheckGamePadNavigation();
  }

  // (render pass): retrieve the next corresponding cached element we
  // created in the layout pass. This is slightly more tricky than a straight
  // lookup because event handlers may insert/remove elements.
  Element *NextElement(const char *id) {
    auto backup = element_it_;
    while (element_it_ != elements_.end()) {
      // This loop usually returns on the first iteration, the only time it
      // doesn't is if an event handler caused an element to removed.
      auto &element = *element_it_;
      ++element_it_;
      if (EqualId(element.id, id)) return &element;
    }
    // Didn't find this id at all, which means an event handler just caused
    // this element to be added, so we skip it.
    element_it_ = backup;
    return nullptr;
  }

  // (layout pass): create a new element.
  void NewElement(const vec2i &size, const char *id) {
    elements_.push_back(Element(size, id));
  }

  // (render pass): move the group's current position past an element of
  // the given size.
  void Advance(const vec2i &size) {
    switch (direction_) {
      case kDirHorizontal:
        position_ += vec2i(size.x() + spacing_, 0);
        break;
      case kDirVertical:
        position_ += vec2i(0, size.y() + spacing_);
        break;
      case kDirOverlay:
        // Keep at starting position.
        break;
    }
  }

  // (render pass): return the position of the current element, as a function
  // of the group's current position and the alignment.
  vec2i Position(const Element &element) {
    auto pos = position_ + margin_.xy();
    auto space = size_ - element.size - margin_.xy() - margin_.zw();
    switch (direction_) {
      case kDirHorizontal:
        pos += AlignDimension(align_, 1, space);
        break;
      case kDirVertical:
        pos += AlignDimension(align_, 0, space);
        break;
      case kDirOverlay:
        pos += AlignDimension(align_, 0, space);
        pos += AlignDimension(align_, 1, space);
        break;
    }
    return pos;
  }

  void RenderQuad(Shader *sh, const vec4 &color, const vec2i &pos,
                  const vec2i &size, const vec4 &uv) {
    renderer_.color() = color;
    sh->Set(renderer_);
    Mesh::RenderAAQuadAlongX(vec3(vec2(pos), 0), vec3(vec2(pos + size), 0),
                             uv.xy(), uv.zw());
  }

  void RenderQuad(Shader *sh, const vec4 &color, const vec2i &pos,
                  const vec2i &size) {
    RenderQuad(sh, color, pos, size, vec4(0, 0, 1, 1));
  }

  // An image element.
  void Image(const char *texture_name, float ysize) {
    auto tex = matman_.FindTexture(texture_name);
    assert(tex);  // You need to have called LoadTexture before.
    if (layout_pass_) {
      auto virtual_image_size =
          vec2(tex->size().x() * ysize / tex->size().y(), ysize);
      // Map the size to real screen pixels, rounding to the nearest int
      // for pixel-aligned rendering.
      auto size = VirtualToPhysical(virtual_image_size);
      NewElement(size, texture_name);
      Extend(size);
    } else {
      auto element = NextElement(texture_name);
      if (element) {
        tex->Set(0);
        RenderQuad(image_shader_, mathfu::kOnes4f, Position(*element),
                   element->size);
        Advance(element->size);
      }
    }
  }

  bool Edit(float ysize, const mathfu::vec2 &edit_size, const char *id,
            std::string *text) {
    StartGroup(GetDirection(kLayoutHorizontalBottom),
               GetAlignment(kLayoutHorizontalBottom), 0, id);
    bool in_edit = false;
    if (EqualId(persistent_.input_focus_, id)) {
      // The widget is in edit.
      in_edit = true;
    }

    // Check event, this marks this element as an interactive element.
    auto event = CheckEvent(false);

    // Set text color
    renderer_.color() = text_color_;

    auto physical_label_size = VirtualToPhysical(edit_size);
    auto size = VirtualToPhysical(vec2(0, ysize));
    auto ui_text = text;
    auto edit_mode = kMultipleLines;
    // Check if the editbox is a single line editbox.
    if (physical_label_size.y() == 0 || physical_label_size.y() == size.y()) {
      physical_label_size.y() = size.y();
      edit_mode = kSingleLine;
    }

    if (in_edit && persistent_.text_edit_.GetEditingText()) {
      // Get a text from the micro editor when it's editing.
      ui_text = persistent_.text_edit_.GetEditingText();
    }
    auto buffer = fontman_.GetBuffer(ui_text->c_str(), ui_text->length(),
                                     size.y(), physical_label_size, true);
    assert(buffer);
    persistent_.text_edit_.SetBuffer(buffer);
    persistent_.text_edit_.SetWindowSize(physical_label_size);

    auto window = vec4i(vec2i(0, 0), physical_label_size);
    if (in_edit) {
      window = persistent_.text_edit_.GetWindow();
    }
    auto pos = Label(ui_text->c_str(), *buffer, window);
    if (!layout_pass_) {
      auto show_caret = false;
      bool pick_caret = event & kEventWentDown;
      if (EqualId(persistent_.input_focus_, id)) {
        // The edit box is in focus. Now we can start text input.
        if (persistent_.input_capture_ != id) {
          // Initialize the editor.
          persistent_.text_edit_.Initialize(id, text, edit_mode);
          persistent_.text_edit_.SetLanguage(fontman_.GetLanguage());
          persistent_.text_edit_.SetBuffer(buffer);
          pick_caret = true;
          CaptureInput(id);
        }
        show_caret = true;
      }
      if (pick_caret) {
        auto caret_pos =
            persistent_.text_edit_.Pick(GetPointerPosition() - pos, 0);
        persistent_.text_edit_.SetCaret(caret_pos);
      }

      int32_t input_region_start, input_region_length;
      int32_t focus_region_start, focus_region_length;
      if (persistent_.text_edit_.GetInputRegions(
              &input_region_start, &input_region_length, &focus_region_start,
              &focus_region_length)) {
        // IME is active in the editor.
        // Show some input region indicators.
        if (show_caret && input_region_length) {
          const float kInputLineWidth = 1.0f;
          const float kFocusLineWidth = 3.0f;

          // Calculate and render an input text region.
          DrawUnderline(*buffer, input_region_start, input_region_length, pos,
                        size.y(), kInputLineWidth);

          // Calculate and render a focus text region inside the input text.
          if (focus_region_length) {
            DrawUnderline(*buffer, focus_region_start, focus_region_length, pos,
                          size.y(), kFocusLineWidth);
          }

          // Specify IME rect to input system.
          auto ime_rect = pos + buffer->GetCaretPosition(input_region_start);
          auto ime_size = pos + buffer->GetCaretPosition(input_region_start +
                                                         input_region_length) -
                          ime_rect;
          if (focus_region_length) {
            ime_rect = pos + buffer->GetCaretPosition(focus_region_start);
            ime_size = pos + buffer->GetCaretPosition(focus_region_start +
                                                      focus_region_length) -
                       ime_rect;
          }
          vec4 rect;
          rect.x() = ime_rect.x();
          rect.y() = ime_rect.y();
          rect.z() = ime_size.x();
          rect.w() = ime_size.y();
          input_.SetTextInputRect(rect);
        }
      }

      if (show_caret) {
        // Render caret.
        const float kCaretPositionSizeFactor = 0.8f;
        auto caret_pos =
            buffer->GetCaretPosition(persistent_.text_edit_.GetCaretPosition());

        auto caret_size = size.y() * kCaretPositionSizeFactor;
        if (caret_pos.x() >= window.x() &&
            caret_pos.x() <= window.x() + window.z() &&
            caret_pos.y() >= window.y() &&
            caret_pos.y() - caret_size <= window.y() + window.w()) {
          caret_pos += pos;
          // Caret Y position is at the base line, add some offset.
          caret_pos.y() -= caret_size;

          RenderCaret(caret_pos, vec2i(1, size.y()));
        }

        // Handle text input events only after the rendering for the pass is
        // finished.
        auto finished_input = persistent_.text_edit_.HandleInputEvents(
            input_.GetTextInputEvents());
        input_.ClearTextInputEvents();
        if (finished_input) {
          CaptureInput(kDummyId);
        }
      }
    }
    EndGroup();
    return in_edit;
  }

  // Helper for Edit widget to draw an underline.
  void DrawUnderline(const FontBuffer &buffer, int32_t start, int32_t length,
                     const vec2i &pos, float font_size, float line_width) {
    const float kUnderlineOffsetFactor = 0.2f;

    auto startpos = buffer.GetCaretPosition(start);
    auto size = buffer.GetCaretPosition(start + length) - startpos;
    startpos.y() += font_size * kUnderlineOffsetFactor;
    size.y() += line_width;

    RenderQuad(color_shader_, mathfu::kOnes4f, pos + startpos, size);
  }

  // Helper for Edit widget to render a caret.
  void RenderCaret(const vec2i &caret_pos, const vec2i &caret_size) {
    // TODO: Make the caret rendering configurable.

    // Caret blink duration.
    // 1.0f/100.0f indicates the counter value increased by 10 for each seconds,
    // so the caret blink cycle becomes 10 / (2 * M_PI) second.
    const float kCareteBlinkDuration = 1.0f / 100.0f;
    auto t = GetTicks();
    if (sinf(static_cast<float>(t) * kCareteBlinkDuration) > 0.0f) {
      RenderQuad(color_shader_, mathfu::kOnes4f, caret_pos, caret_size);
    }
  }

  // Text label.
  void Label(const char *text, float ysize) {
    auto size = vec2(0, ysize);
    Label(text, ysize, size);
  }

  // Multi line Text label.
  void Label(const char *text, float ysize, const vec2 &label_size) {
    // Set text color.
    renderer_.color() = text_color_;

    auto physical_label_size = VirtualToPhysical(label_size);
    auto size = VirtualToPhysical(vec2(0, ysize));
    auto buffer = fontman_.GetBuffer(text, strlen(text), size.y(),
                                     physical_label_size, false);
    assert(buffer);
    Label(text, *buffer, vec4i(vec2i(0, 0), buffer->get_size()));
  }

  vec2i Label(const char *text, const FontBuffer &buffer, const vec4i &window) {
    vec2i pos = mathfu::kZeros2i;
    if (layout_pass_) {
      auto size = window.zw();
      NewElement(size, text);
      Extend(size);
    } else {
      // Check if texture atlas needs to be updated.
      if (buffer.get_pass() > 0) {
        fontman_.StartRenderPass();
      }

      auto element = NextElement(text);
      if (element) {
        fontman_.GetAtlasTexture()->Set(0);

        pos = Position(*element);

        bool clipping = false;
        if (window.z() && window.w()) {
          clipping = window.x() || window.y() ||
                     (buffer.get_size().x() > window.z()) ||
                     (buffer.get_size().y() > window.w());
        }
        if (clipping) {
          pos -= window.xy();

          // Set a window to show a part of the label.
          font_clipping_shader_->Set(renderer_);
          font_clipping_shader_->SetUniform("pos_offset",
                                            vec3(pos.x(), pos.y(), 0.0f));
          auto start = vec2(position_ - pos);
          auto end = start + vec2(window.zw());
          font_clipping_shader_->SetUniform("clipping", vec4(start, end));
        } else {
          font_shader_->Set(renderer_);
          font_shader_->SetUniform("pos_offset", vec3(pos.x(), pos.y(), 0.0f));
        }

        const Attribute kFormat[] = {kPosition3f, kTexCoord2f, kEND};
        Mesh::RenderArray(
            GL_TRIANGLES, buffer.get_indices()->size(), kFormat,
            sizeof(FontVertex),
            reinterpret_cast<const char *>(buffer.get_vertices()->data()),
            buffer.get_indices()->data());
        Advance(element->size);
      }
    }
    return pos;
  }

  // Custom element with user supplied renderer.
  void CustomElement(
      const vec2 &virtual_size, const char *id,
      const std::function<void(const vec2i &pos, const vec2i &size)> renderer) {
    if (layout_pass_) {
      auto size = VirtualToPhysical(virtual_size);
      NewElement(size, id);
      Extend(size);
    } else {
      auto element = NextElement(id);
      if (element) {
        renderer(Position(*element), element->size);
        Advance(element->size);
      }
    }
  }

  // Render texture on the screen.
  void RenderTexture(const Texture &tex, const vec2i &pos, const vec2i &size) {
    if (!layout_pass_) {
      tex.Set(0);
      RenderQuad(image_shader_, mathfu::kOnes4f, pos, size);
    }
  }

  void RenderTextureNinePatch(const Texture &tex, const vec4 &patch_info,
                              const vec2i &pos, const vec2i &size) {
    if (!layout_pass_) {
      tex.Set(0);
      renderer_.color() = mathfu::kOnes4f;
      image_shader_->Set(renderer_);
      Mesh::RenderAAQuadAlongXNinePatch(vec3(vec2(pos), 0),
                                        vec3(vec2(pos + size), 0), tex.size(),
                                        patch_info);
    }
  }

  // An element that has sub-elements. Tracks its state in an instance of
  // Layout, that is pushed/popped from the stack as needed.
  void StartGroup(Direction direction, Alignment align, float spacing,
                  const char *id) {
    Group layout(direction, align, spacing, elements_.size());
    group_stack_.push_back(*this);
    if (layout_pass_) {
      NewElement(mathfu::kZeros2i, id);
    } else {
      auto element = NextElement(id);
      if (element) {
        layout.position_ = Position(*element);
        layout.size_ = element->size;
        // Make layout refer to element it originates from, iterator points
        // to next element after the current one.
        layout.element_idx_ = element_it_ - elements_.begin() - 1;
      } else {
        // This group did not exist during layout, but since all code inside
        // this group will run, it is important to have a valid element_idx_
        // to refer to, so we point it to our (empty) sentinel element:
        layout.element_idx_ = elements_.size() - 1;
      }
    }
    *static_cast<Group *>(this) = layout;

    // Reset clipping status.
    clip_position_ = mathfu::kZeros2i;
    clip_size_ = mathfu::kZeros2i;
  }

  // Clean up the Group element started by StartGroup()
  void EndGroup() {
    // If you hit this assert, you have one too many EndGroup().
    assert(group_stack_.size());

    auto size = size_;
    auto margin = margin_.xy() + margin_.zw();
    auto element_idx = element_idx_;
    *static_cast<Group *>(this) = group_stack_.back();
    group_stack_.pop_back();
    if (layout_pass_) {
      size += margin;
      // Contribute the size of this group to its parent.
      Extend(size);
      // Set the size of this group as the size of the element tracking it.
      elements_[element_idx].size = size;
      // TODO: we currently just make the last group in any overlay group
      // the one to receive events. This is sufficient for popups, but it be
      // better if this could also be specified manually.
      if (direction_ == kDirOverlay) {
        // Simply mark all elements before this last group as non-interactive.
        for (size_t i = 0; i < element_idx; i++)
          elements_[i].interactive = false;
      }
    } else {
      Advance(elements_[element_idx].size);
    }
  }

  void SetMargin(const Margin &margin) {
    margin_ = VirtualToPhysical(margin.borders);
  }

  void StartScroll(const vec2 &size, vec2i *offset) {
    auto psize = VirtualToPhysical(size);

    if (layout_pass_) {
      // If you hit this assert, you are nesting scrolling areas, which is
      // not supported.
      assert(!clip_inside_);
      clip_inside_ = true;
      // Pass this size to EndScroll.
      clip_size_ = psize;
    } else {
      // This currently assumes an ortho camera that corresponds to all pixels
      // of the GL screen, which is exactly what Run() sets up.
      // If that ever changes, this call will have to get more complicated,
      // translating to whereever the GUI is placed, or in the case of 3D
      // placement use another technique alltogether (render to texture,
      // glClipPlane, or stencil buffer).
      renderer_.ScissorOn(
            vec2i(position_.x(),
                  renderer_.window_size().y() - position_.y() - psize.y()),
            psize);

      vec2i pointer_delta = mathfu::kZeros2i;
      int32_t scroll_speed = scroll_speed_drag_;

      // Check drag event only.
      auto &element = elements_[element_idx_];
      size_ = psize;

      // Set the interactive flag, check event and restore the flag.
      auto interactive = element.interactive;
      element.interactive = true;
      auto event = CheckEvent(true);
      element.interactive = interactive;

      if (event & kEventStartDrag) {
        // Start drag.
        CapturePointer(element.id);
      }

      if (IsPointerCaptured(element.id)) {
        if (event & kEventEndDrag) {
          // Finish dragging and release the pointer.
          ReleasePointer();
        }
        pointer_delta = input_.get_pointers()[0].mousedelta;
      } else {
        if (mathfu::InRange2D(input_.get_pointers()[0].mousepos, position_,
                              position_ + psize)) {
          pointer_delta = input_.mousewheel_delta();
          scroll_speed = -scroll_speed_wheel_;
        }
      }

      // Scroll the pane on user input.
      *offset = vec2i::Min(
          elements_[element_idx_].extra_size,
          vec2i::Max(mathfu::kZeros2i, *offset - pointer_delta * scroll_speed));

      // See if the mouse is outside the clip area, so we can avoid events
      // being triggered by elements that are not visible.
      for (int i = 0; i <= pointer_max_active_index_; i++) {
        if (!mathfu::InRange2D(input_.get_pointers()[i].mousepos, position_,
                               position_ + psize)) {
          clip_mouse_inside_[i] = false;
        }
      }
      // Store size/position, so expensive rendering commands can choose to
      // clip against the viewport.
      // TODO: add culling code where appropriate.
      clip_size_ = psize;
      clip_position_ = position_;
      // Start the rendering of this group at the offset before the start of
      // the window to clip against. Also makes events work correctly.
      position_ -= *offset;
    }
  }

  void EndScroll() {
    if (layout_pass_) {
      // Track original size.
      elements_[element_idx_].extra_size = size_ - clip_size_;
      // Overwrite what was computed for the elements.
      size_ = clip_size_;
      clip_inside_ = false;
    } else {
      for (int i = 0; i <= pointer_max_active_index_; i++) {
        clip_mouse_inside_[i] = true;
      }
      renderer_.ScissorOff();
    }
  }

  void StartSlider(Direction direction, float *value) {
    auto event = CheckEvent(false);
    if (!layout_pass_) {
      if (event & kEventStartDrag) {
        CapturePointer(elements_[element_idx_].id);
      } else if (event & kEventEndDrag) {
        ReleasePointer();
      }
      // Update the knob position.
      if (event & kEventIsDragging || event & kEventWentDown ||
          event & kEventIsDown) {
        switch (direction) {
          case kDirHorizontal:
            *value = static_cast<float>(GetPointerPosition().x() -
                                        position_.x() - size_.y() * 0.5f) /
                     static_cast<float>(size_.x() - size_.y());
            break;
          case kDirVertical:
            *value = static_cast<float>(GetPointerPosition().y() -
                                        position_.y() - size_.x() * 0.5f) /
                     static_cast<float>(size_.y() - size_.x());
            break;
          default:
            assert(0);
        }
        // Clamp the slider value.
        *value = std::max(0.0f, std::min(1.0f, *value));
        fpl::LogInfo("Changed Slider Value:%f", *value);
      }
    }
  }

  void EndSlider() {}

  // Set scroll speed of the scroll group.
  // scroll_speed_drag: Scroll speed with a pointer drag operation.
  // scroll_speed_wheel: Scroll speed with a mouse wheel operation.
  void SetScrollSpeed(float scroll_speed_drag, float scroll_speed_wheel) {
    scroll_speed_drag_ = scroll_speed_drag;
    scroll_speed_wheel_ = scroll_speed_wheel;
  }

  // Set drag start threshold.
  // The value is used to determine if the drag operation should start after a
  // pointer WENT_DOWN event happened.
  void SetDragStartThreshold(float drag_start_threshold) {
    drag_start_threshold_ = vec2i(drag_start_threshold, drag_start_threshold);
  }

  // Capture the input to an element.
  void CaptureInput(const char *element_id) {
    persistent_.input_capture_ = element_id;
    if (element_id != kDummyId) {
      // Start recording input events.
      if (!input_.IsRecordingTextInput()) {
        input_.RecordTextInput(true);
      }

      // Enable IME.
      input_.StartTextInput();
    } else {
      // The element releases keyboard focus as well.
      persistent_.input_focus_ = kDummyId;

      // Stop recording input events.
      if (input_.IsRecordingTextInput()) {
        input_.RecordTextInput(false);
      }
      // Disable IME
      input_.StopTextInput();
    }
  }

  // Capture the pointer to an element.
  // The element with element_id will continue to recieve pointer events
  // exclusively until CapturePointer() with kDummyId API is called.
  //
  // element_id: an element ID that captures the pointer.
  void CapturePointer(const char *element_id) {
    persistent_.mouse_capture_ = element_id;
    RecordId(element_id, current_pointer_);
  }

  // Check if the element can recieve pointer events.
  // Return false if the pointer is captured by another element.
  bool CanReceivePointerEvent(const char *element_id) {
    return persistent_.mouse_capture_ == kDummyId ||
           EqualId(persistent_.mouse_capture_, element_id);
  }

  // Check if the element is capturing a pointer events.
  bool IsPointerCaptured(const char *element_id) {
    return EqualId(persistent_.mouse_capture_, element_id);
  }

  vec2i GroupSize() { return size_ + elements_[element_idx_].extra_size; }

  void RecordId(const char *id, int i) { persistent_.pointer_element[i] = id; }
  bool SameId(const char *id, int i) {
    return EqualId(id, persistent_.pointer_element[i]);
  }

  Event CheckEvent(bool check_dragevent_only) {
    auto &element = elements_[element_idx_];
    if (layout_pass_) {
      element.interactive = true;
    } else {
      // We only fire events after the layout pass.
      // Check if this is an inactive part of an overlay.
      if (element.interactive) {
        auto id = element.id;

        // pointer_max_active_index_ is typically 0, so loop not expensive.
        for (int i = 0; i <= pointer_max_active_index_; i++) {
          if ((CanReceivePointerEvent(id) && clip_mouse_inside_[i] &&
               mathfu::InRange2D(input_.get_pointers()[i].mousepos, position_,
                                 position_ + size_)) ||
              IsPointerCaptured(id)) {
            auto &button = *pointer_buttons_[i];
            int event = 0;

            if (persistent_.dragging_pointer_ == i) {
              // The pointer is in drag operation.
              if (button.went_up()) {
                event |= kEventEndDrag;
                persistent_.dragging_pointer_ = kPointerIndexInvalid;
                persistent_.drag_start_position_ = kDragStartPoisitionInvalid;
              } else if (button.is_down()) {
                event |= kEventIsDragging;
              }
            } else {
              if (!check_dragevent_only) {
                // Regular pointer event handlings.
                if (button.went_down()) {
                  RecordId(id, i);
                  event |= kEventWentDown;
                }

                if (button.went_up() && SameId(id, i)) {
                  event |= kEventWentUp;
                } else if (button.is_down() && SameId(id, i)) {
                  event |= kEventIsDown;

                  if (persistent_.input_focus_ != id) {
                    // Stop input handling.
                    CaptureInput(kDummyId);
                    // Record the last element we received an up on, as the
                    // target for keyboard input.
                    persistent_.input_focus_ = id;
                  }
                }
              }

              // Check for drag events.
              if (button.went_down()) {
                persistent_.drag_start_position_ =
                    input_.get_pointers()[i].mousepos;
              }
              if (button.is_down() &&
                  mathfu::InRange2D(persistent_.drag_start_position_, position_,
                                    position_ + size_) &&
                  !mathfu::InRange2D(
                       input_.get_pointers()[i].mousepos,
                       persistent_.drag_start_position_ - drag_start_threshold_,
                       persistent_.drag_start_position_ +
                           drag_start_threshold_)) {
                // Start drag event.
                // Note that any element the event can recieve the drag start
                // event, so that parent layer can start a dragging operation
                // regardless if it's sub-layer is checking event.
                event |= kEventStartDrag;
                persistent_.drag_start_position_ =
                    input_.get_pointers()[i].mousepos;
                persistent_.dragging_pointer_ = i;
              }
            }

            if (!event) event = kEventHover;

            gamepad_has_focus_element = true;
            current_pointer_ = i;
            // We only report an event for the first finger to touch an element.
            // This is intentional.
            return static_cast<Event>(event);
          }
        }
        // Generate hover events for the current element the gamepad is focused
        // on.
        if (EqualId(persistent_.input_focus_, id)) {
          gamepad_has_focus_element = true;
          return gamepad_event;
        }
      }
    }
    return kEventNone;
  }

  void CheckGamePadFocus() {
    if (!gamepad_has_focus_element)
      // This may happen when a GUI first appears or when elements get removed.
      // TODO: only do this when there's an actual gamepad connected.
      persistent_.input_focus_ = NextInteractiveElement(-1, 1);
  }

  void CheckGamePadNavigation() {
    // Gamepad/keyboard navigation only happens when the keyboard is not
    // captured.
    if (persistent_.input_capture_ != kDummyId) {
      return;
    }

    int dir = 0;
// FIXME: this should work on other platforms too.
#ifdef ANDROID_GAMEPAD
    auto &gamepads = input_.GamepadMap();
    for (auto &gamepad : gamepads) {
      dir = CheckButtons(gamepad.second.GetButton(Gamepad::kLeft),
                         gamepad.second.GetButton(Gamepad::kRight),
                         gamepad.second.GetButton(Gamepad::kButtonA));
    }
#endif
    // For testing, also support keyboard:
    dir =
        CheckButtons(input_.GetButton(SDLK_LEFT), input_.GetButton(SDLK_RIGHT),
                     input_.GetButton(SDLK_RETURN));
    // Now find the current element, and move to the next.
    if (dir) {
      for (auto &e : elements_) {
        if (EqualId(e.id, persistent_.input_focus_)) {
          persistent_.input_focus_ =
              NextInteractiveElement(&e - &elements_[0], dir);
          break;
        }
      }
    }
  }

  int CheckButtons(const Button &left, const Button &right,
                   const Button &action) {
    int dir = 0;
    if (left.went_up()) dir = -1;
    if (right.went_up()) dir = 1;
    if (action.went_up()) gamepad_event = kEventWentUp;
    if (action.went_down()) gamepad_event = kEventWentDown;
    if (action.is_down()) gamepad_event = kEventIsDown;
    return dir;
  }

  const char *NextInteractiveElement(int start, int direction) {
    auto range = static_cast<int>(elements_.size());
    for (auto i = start;;) {
      i += direction;
      // Wrap around.. just once.
      if (i < 0)
        i = range - 1;
      else if (i >= range)
        i = -1;
      // Back where we started, either there's no interactive elements, or
      // the vector is empty.
      if (i == start) return kDummyId;
      if (elements_[i].interactive) return elements_[i].id;
    }
  }

  void ColorBackground(const vec4 &color) {
    if (!layout_pass_) {
      RenderQuad(color_shader_, color, position_, GroupSize());
    }
  }

  void ImageBackground(const Texture &tex) {
    if (!layout_pass_) {
      tex.Set(0);
      RenderQuad(image_shader_, mathfu::kOnes4f, position_, GroupSize());
    }
  }

  void ImageBackgroundNinePatch(const Texture &tex, const vec4 &patch_info) {
    RenderTextureNinePatch(tex, patch_info, position_, GroupSize());
  }

  // Set Label's text color.
  void SetTextColor(const vec4 &color) { text_color_ = color; }

 private:
  vec2i GetPointerDelta() { return input_.get_pointers()[0].mousedelta; }

  vec2i GetPointerPosition() { return input_.get_pointers()[0].mousepos; }

  bool layout_pass_;
  std::vector<Element> elements_;
  std::vector<Element>::iterator element_it_;
  std::vector<Group> group_stack_;
  vec2i canvas_size_;
  float virtual_resolution_;
  float pixel_scale_;

  AssetManager &matman_;
  Renderer &renderer_;
  InputSystem &input_;
  FontManager &fontman_;
  Shader *image_shader_;
  Shader *font_shader_;
  Shader *font_clipping_shader_;
  Shader *color_shader_;

  // Expensive rendering commands can check if they're inside this rect to
  // cull themselves inside a scrolling group.
  vec2i clip_position_;
  vec2i clip_size_;
  bool clip_mouse_inside_[InputSystem::kMaxSimultanuousPointers];
  bool clip_inside_;

  // Widget properties.
  mathfu::vec4 text_color_;

  int pointer_max_active_index_;
  const Button *pointer_buttons_[InputSystem::kMaxSimultanuousPointers];
  bool gamepad_has_focus_element;
  Event gamepad_event;

  // Drag operations.
  float scroll_speed_drag_;
  float scroll_speed_wheel_;
  vec2i drag_start_threshold_;

  // The latest pointer that returned an event.
  int32_t current_pointer_;

  // Intra-frame persistent state.
  static struct PersistentState {
    PersistentState() {
      // This is effectively a global, so no memory allocation or other
      // complex initialization here.
      for (int i = 0; i < InputSystem::kMaxSimultanuousPointers; i++) {
        pointer_element[i] = kDummyId;
      }
      input_focus_ = input_capture_ = mouse_capture_ = kDummyId;
      dragging_pointer_ = kPointerIndexInvalid;
    }

    // For each pointer, the element id that last received a down event.
    const char *pointer_element[InputSystem::kMaxSimultanuousPointers];
    // The element the gamepad is currently "over", simulates the mouse
    // hovering over an element.
    const char *input_focus_;

    // The element that capturing the keyboard.
    const char *input_capture_;

    // The element that capturing the pointer.
    // The element continues to recieve mouse events until it releases the
    // capture.
    const char *mouse_capture_;

    // Simple text edit handler for an edit box.
    MicroEdit text_edit_;

    // Keep tracking a pointer position of a drag start.
    vec2i drag_start_position_;
    int32_t dragging_pointer_;
  } persistent_;
};

InternalState::PersistentState InternalState::persistent_;

void Run(AssetManager &assetman, FontManager &fontman, InputSystem &input,
         const std::function<void()> &gui_definition) {
  // Create our new temporary state.
  InternalState internal_state(assetman, fontman, input);

  // Run two passes, one for layout, one for rendering.
  // First pass:
  gui_definition();

  // Second pass:
  internal_state.StartRenderPass();

  internal_state.SetOrtho();

  auto &renderer = assetman.renderer();
  renderer.SetBlendMode(kBlendModeAlpha);
  renderer.DepthTest(false);

  gui_definition();

  internal_state.CheckGamePadFocus();
}

InternalState *Gui() {
  assert(state);
  return state;
}

void Image(const char *texture_name, float size) {
  Gui()->Image(texture_name, size);
}

void Label(const char *text, float font_size) { Gui()->Label(text, font_size); }

void Label(const char *text, float font_size, const vec2 &size) {
  Gui()->Label(text, font_size, size);
}

bool Edit(float ysize, const mathfu::vec2 &size, const char *id,
          std::string *string) {
  return Gui()->Edit(ysize, size, id, string);
}

void StartGroup(Layout layout, float spacing, const char *id) {
  Gui()->StartGroup(GetDirection(layout), GetAlignment(layout), spacing, id);
}

void EndGroup() { Gui()->EndGroup(); }

void SetMargin(const Margin &margin) { Gui()->SetMargin(margin); }

void StartScroll(const vec2 &size, vec2i *offset) {
  Gui()->StartScroll(size, offset);
}

void EndScroll() { Gui()->EndScroll(); }

void StartSlider(Direction direction, float *value) {
  Gui()->StartSlider(direction, value);
}

void EndSlider() { Gui()->EndSlider(); }

void CustomElement(
    const vec2 &virtual_size, const char *id,
    const std::function<void(const vec2i &pos, const vec2i &size)> renderer) {
  Gui()->CustomElement(virtual_size, id, renderer);
}

void RenderTexture(const Texture &tex, const vec2i &pos, const vec2i &size) {
  Gui()->RenderTexture(tex, pos, size);
}

void RenderTextureNinePatch(const Texture &tex, const vec4 &patch_info,
                            const vec2i &pos, const vec2i &size) {
  Gui()->RenderTextureNinePatch(tex, patch_info, pos, size);
}

void SetTextColor(const mathfu::vec4 &color) { Gui()->SetTextColor(color); }

Event CheckEvent() { return Gui()->CheckEvent(false); }
Event CheckEvent(bool check_dragevent_only) {
  return Gui()->CheckEvent(check_dragevent_only);
}

void ColorBackground(const vec4 &color) { Gui()->ColorBackground(color); }

void ImageBackground(const Texture &tex) { Gui()->ImageBackground(tex); }

void ImageBackgroundNinePatch(const Texture &tex, const vec4 &patch_info) {
  Gui()->ImageBackgroundNinePatch(tex, patch_info);
}

void PositionUI(float virtual_resolution, Alignment horizontal, Alignment vertical) {
  Gui()->PositionUI(virtual_resolution, horizontal, vertical);
}

mathfu::vec2i VirtualToPhysical(const mathfu::vec2 &v) {
  return Gui()->VirtualToPhysical(v);
}

float GetScale() { return Gui()->GetScale(); }

void CapturePointer(const char *element_id) {
  Gui()->CapturePointer(element_id);
}

void ReleasePointer() { Gui()->CapturePointer(kDummyId); }

void SetScrollSpeed(float scroll_speed_drag, float scroll_speed_wheel) {
  Gui()->SetScrollSpeed(scroll_speed_drag, scroll_speed_wheel);
}

void SetDragStartThreshold(float drag_start_threshold) {
  Gui()->SetDragStartThreshold(drag_start_threshold);
}

}  // namespace gui
}  // namespace fpl