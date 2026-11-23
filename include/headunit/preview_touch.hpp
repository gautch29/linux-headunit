// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/touch_input.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace headunit {
// Surface coordinates and the phone's HID coordinates are separate domains.
// Match the embedded video's aspect-preserving rectangle, including bars.
class PreviewTouch {
 public:
  struct Rectangle {
    int x, y, width, height;
  };
  PreviewTouch(unsigned width, unsigned height) : width_(width), height_(height) {
    if (width < 320 || width > 8192 || height < 240 || height > 4320)
      throw std::invalid_argument("invalid preview touch dimensions");
    resize(width, height);
  }
  void resize(unsigned width, unsigned height) {
    if (!width || !height || width > 16384 || height > 16384)
      throw std::invalid_argument("invalid preview surface dimensions");
    cancel();
    const double scale = std::min(double(width) / width_, double(height) / height_);
    const int w = std::max(1, int(std::floor(width_ * scale)));
    const int h = std::max(1, int(std::floor(height_ * scale)));
    rectangle_ = {int(width - w) / 2, int(height - h) / 2, w, h};
  }
  Rectangle rectangle() const { return rectangle_; }
  // Up to two fingers with multitouch, otherwise the first finger only. Every
  // change reports the complete state of both contacts.
  void down(int id, double x, double y) {
    if (!valid(x, y) || x < rectangle_.x || y < rectangle_.y ||
        x >= rectangle_.x + rectangle_.width || y >= rectangle_.y + rectangle_.height)
      return;
    const std::size_t slots = multitouch_ ? 2 : 1;
    for (std::size_t slot = 0; slot < slots; ++slot)
      if (contacts_[slot].id == id) return;
    for (std::size_t slot = 0; slot < slots; ++slot)
      if (!contacts_[slot].id) {
        contacts_[slot].id = id;
        place(slot, x, y);
        enqueue(state());
        return;
      }
  }
  void motion(int id, double x, double y) {
    if (!valid(x, y)) return;
    for (std::size_t slot = 0; slot < contacts_.size(); ++slot)
      if (contacts_[slot].id == id) {
        place(slot, x, y);
        enqueue(state());
      }
  }
  void up(int id) {
    for (auto& contact : contacts_)
      if (contact.id == id) {
        contact.id.reset();
        enqueue(state());
      }
  }
  void cancel() {
    if (!contacts_[0].id && !contacts_[1].id) return;
    for (auto& contact : contacts_)
      contact.id.reset();
    enqueue(state());
  }
  std::vector<TouchInput> take() {
    std::vector<TouchInput> result;
    result.swap(pending_);
    return result;
  }

 private:
  struct Contact {
    std::optional<int> id;
    std::uint16_t x = 0, y = 0;
  };
  static bool valid(double x, double y) { return std::isfinite(x) && std::isfinite(y); }
  void enqueue(TouchInput input) {
    if (pending_.size() == 64) {
      // End the gesture on overload, even if the phone already saw its press.
      pending_.clear();
      for (auto& contact : contacts_)
        contact.id.reset();
      input.pressed = false;
      input.second_pressed = false;
    }
    pending_.push_back(input);
  }
  void place(std::size_t slot, double x, double y) {
    contacts_[slot].x = static_cast<std::uint16_t>(
        std::clamp((x - rectangle_.x) * width_ / rectangle_.width, 0.0, double(width_ - 1)));
    contacts_[slot].y = static_cast<std::uint16_t>(
        std::clamp((y - rectangle_.y) * height_ / rectangle_.height, 0.0, double(height_ - 1)));
  }
  TouchInput state() const {
    return {bool(contacts_[0].id), contacts_[0].x, contacts_[0].y,
            bool(contacts_[1].id), contacts_[1].x, contacts_[1].y};
  }
  unsigned width_, height_;
  Rectangle rectangle_{};
  bool multitouch_ = multitouch_enabled();
  std::array<Contact, 2> contacts_{};
  std::vector<TouchInput> pending_;
};
}  // namespace headunit
