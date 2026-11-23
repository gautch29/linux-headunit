// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/backup_cluster.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <ostream>
#include <cstdio>
#include <string>
#include <stdexcept>

namespace headunit {
namespace {
constexpr Rgb background{7, 13, 23};
constexpr Rgb dim{37, 48, 63};
constexpr Rgb pale{225, 236, 245};
constexpr Rgb speed_color{79, 187, 251};
constexpr Rgb rpm_color{132, 221, 158};
constexpr auto maximum_sample_age = std::chrono::milliseconds(500);

void rectangle(Image& image, int x, int y, int w, int h, Rgb color) {
  const int left = std::max(0, x), top = std::max(0, y);
  const int right = std::min(static_cast<int>(image.width), x + w);
  const int bottom = std::min(static_cast<int>(image.height), y + h);
  for (int row = top; row < bottom; ++row) {
    for (int col = left; col < right; ++col) {
      image.pixels[static_cast<std::size_t>(row) * image.width + col] = color;
    }
  }
}

// A deliberately small seven-segment font keeps the fallback independent of
// fonts, asset packages, phone rendering, and GPU initialization.
constexpr std::array<std::uint8_t, 10> digits{0b0111111, 0b0000110, 0b1011011, 0b1001111,
                                              0b1100110, 0b1101101, 0b1111101, 0b0000111,
                                              0b1111111, 0b1101111};

void digit(Image& image, int x, int y, int scale, int value, Rgb color) {
  if (value == -2) return;  // Leading blank; -1 is unavailable.
  const auto segments = value < 0 ? std::uint8_t{0b1000000} : digits.at(value);
  const int t = std::max(2, scale / 5), length = scale;
  if (segments & 1) rectangle(image, x + t, y, length, t, color);
  if (segments & 2) rectangle(image, x + length + t, y + t, t, length, color);
  if (segments & 4) rectangle(image, x + length + t, y + length + 2 * t, t, length, color);
  if (segments & 8) rectangle(image, x + t, y + 2 * length + 2 * t, length, t, color);
  if (segments & 16) rectangle(image, x, y + length + 2 * t, t, length, color);
  if (segments & 32) rectangle(image, x, y + t, t, length, color);
  if (segments & 64) rectangle(image, x + t, y + length + t, length, t, color);
}

void number(Image& image, int x, int y, int scale, int value, int places, Rgb color) {
  const int step = scale + 2 * std::max(2, scale / 5) + scale / 2;
  for (int i = places - 1; i >= 0; --i) {
    const int place = value < 0 ? -1 : (value == 0 && i < places - 1 ? -2 : value % 10);
    digit(image, x + i * step, y, scale, place, color);
    if (value >= 0) value /= 10;
  }
}

void letter(Image& image, int x, int y, int scale, char symbol, Rgb color) {
  using Glyph = std::array<std::uint8_t, 7>;
  Glyph rows{};
  switch (symbol) {
    case 'A':
      rows = {14, 17, 17, 31, 17, 17, 17};
      break;
    case 'B':
      rows = {30, 17, 17, 30, 17, 17, 30};
      break;
    case 'C':
      rows = {14, 17, 16, 16, 16, 17, 14};
      break;
    case 'D':
      rows = {30, 17, 17, 17, 17, 17, 30};
      break;
    case 'E':
      rows = {31, 16, 16, 30, 16, 16, 31};
      break;
    case 'F':
      rows = {31, 16, 16, 30, 16, 16, 16};
      break;
    case 'G':
      rows = {14, 17, 16, 23, 17, 17, 15};
      break;
    case 'H':
      rows = {17, 17, 17, 31, 17, 17, 17};
      break;
    case 'I':
      rows = {14, 4, 4, 4, 4, 4, 14};
      break;
    case 'J':
      rows = {7, 2, 2, 2, 18, 18, 12};
      break;
    case 'K':
      rows = {17, 18, 20, 24, 20, 18, 17};
      break;
    case 'L':
      rows = {16, 16, 16, 16, 16, 16, 31};
      break;
    case 'M':
      rows = {17, 27, 21, 21, 17, 17, 17};
      break;
    case 'N':
      rows = {17, 25, 21, 19, 17, 17, 17};
      break;
    case 'O':
      rows = {14, 17, 17, 17, 17, 17, 14};
      break;
    case 'P':
      rows = {30, 17, 17, 30, 16, 16, 16};
      break;
    case 'Q':
      rows = {14, 17, 17, 17, 21, 18, 13};
      break;
    case 'R':
      rows = {30, 17, 17, 30, 20, 18, 17};
      break;
    case 'S':
      rows = {15, 16, 16, 14, 1, 1, 30};
      break;
    case 'T':
      rows = {31, 4, 4, 4, 4, 4, 4};
      break;
    case 'U':
      rows = {17, 17, 17, 17, 17, 17, 14};
      break;
    case 'V':
      rows = {17, 17, 17, 17, 17, 10, 4};
      break;
    case 'W':
      rows = {17, 17, 17, 21, 21, 21, 10};
      break;
    case 'X':
      rows = {17, 17, 10, 4, 10, 17, 17};
      break;
    case 'Y':
      rows = {17, 17, 10, 4, 4, 4, 4};
      break;
    case 'Z':
      rows = {31, 1, 2, 4, 8, 16, 31};
      break;
    case '0':
      rows = {14, 17, 19, 21, 25, 17, 14};
      break;
    case '1':
      rows = {4, 12, 4, 4, 4, 4, 14};
      break;
    case '2':
      rows = {14, 17, 1, 2, 4, 8, 31};
      break;
    case '3':
      rows = {30, 1, 1, 14, 1, 1, 30};
      break;
    case '4':
      rows = {2, 6, 10, 18, 31, 2, 2};
      break;
    case '5':
      rows = {31, 16, 16, 30, 1, 1, 30};
      break;
    case '6':
      rows = {14, 16, 16, 30, 17, 17, 14};
      break;
    case '7':
      rows = {31, 1, 2, 4, 8, 8, 8};
      break;
    case '8':
      rows = {14, 17, 17, 14, 17, 17, 14};
      break;
    case '9':
      rows = {14, 17, 17, 15, 1, 1, 14};
      break;
    case '-':
      rows = {0, 0, 0, 31, 0, 0, 0};
      break;
    case '!':
      rows = {4, 4, 4, 4, 4, 0, 4};
      break;
    case '?':
      rows = {14, 17, 1, 2, 4, 0, 4};
      break;
    case '.':
      rows = {0, 0, 0, 0, 0, 0, 4};
      break;
    case '/':
      rows = {1, 2, 2, 4, 8, 8, 16};
      break;
    case '%':
      rows = {25, 25, 2, 4, 8, 19, 19};
      break;
    default:
      return;
  }
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 5; ++col) {
      if (rows[row] & (1 << (4 - col))) {
        rectangle(image, x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

void label(Image& image, int x, int y, int scale, std::string_view text, Rgb color = pale) {
  for (std::size_t i = 0; i < text.size(); ++i)
    letter(image, x + int(i) * 6 * scale, y, scale, text[i], color);
}
Rgb indicator_color(IndicatorColor color) {
  switch (color) {
    case IndicatorColor::red:
      return {255, 83, 88};
    case IndicatorColor::amber:
      return {255, 190, 65};
    case IndicatorColor::blue:
      return {70, 165, 255};
    case IndicatorColor::green:
      return {100, 225, 150};
  }
  return pale;
}
void marker(Image& image, int x, int y, Indicator id, Rgb color) {
  // Original schematic telltales; text remains present for unambiguous labels.
  if (id == Indicator::left_turn || id == Indicator::right_turn) {
    rectangle(image, x + 3, y + 6, 17, 4, color);
    for (int i = 0; i < 7; ++i) {
      const int px = id == Indicator::left_turn ? x + i : x + 22 - i;
      rectangle(image, px, y + 7 - i, 2, 2 + i * 2, color);
    }
  } else if (id == Indicator::battery_charge) {
    rectangle(image, x + 2, y + 2, 19, 13, color);
    rectangle(image, x + 4, y + 4, 15, 9, background);
    rectangle(image, x + 5, y, 4, 2, color);
    rectangle(image, x + 15, y, 4, 2, color);
    rectangle(image, x + 6, y + 7, 5, 1, color);
    rectangle(image, x + 8, y + 5, 1, 5, color);
    rectangle(image, x + 14, y + 7, 4, 1, color);
  } else if (id == Indicator::check_engine) {
    rectangle(image, x + 1, y + 5, 20, 10, color);
    rectangle(image, x + 4, y + 2, 11, 13, color);
    rectangle(image, x + 4, y + 7, 14, 5, background);
    rectangle(image, x + 22, y + 7, 2, 6, color);
  } else if (id == Indicator::high_beam || id == Indicator::low_beam ||
             id == Indicator::front_fog || id == Indicator::rear_fog) {
    rectangle(image, x + 2, y + 3, 9, 11, color);
    rectangle(image, x + 4, y + 5, 5, 7, background);
    for (int i = 0; i < 3; ++i)
      rectangle(image, x + 14, y + 3 + i * 5, 10, 2, color);
  } else if (id == Indicator::coolant_warning) {
    rectangle(image, x + 8, y, 4, 12, color);
    rectangle(image, x + 5, y + 10, 10, 6, color);
    for (int i = 0; i < 3; ++i)
      rectangle(image, x + 16, y + 2 + i * 4, 5, 2, color);
  } else {
    rectangle(image, x + 5, y + 1, 14, 14, color);
    rectangle(image, x + 7, y + 3, 10, 10, background);
    label(image, x + 10, y + 4, 1,
          id == Indicator::parking_brake  ? "P"
          : id == Indicator::oil_pressure ? "O"
                                          : "!",
          color);
  }
}

}  // namespace

Rgb Image::at(unsigned x, unsigned y) const {
  if (x >= width || y >= height) throw std::out_of_range("pixel outside image");
  return pixels[static_cast<std::size_t>(y) * width + x];
}

Image render_backup_cluster(const VehicleSample& source, Clock::time_point now, unsigned width,
                            unsigned height) {
  if (width < 640 || width > 4096 || height < 240 || height > 2160) {
    throw std::invalid_argument("cluster dimensions outside supported range");
  }
  Image image{960, 360, std::vector<Rgb>(960 * 360, background)};
  const auto sample = current_vehicle_sample(source, now, maximum_sample_age);
  const bool valid = fresh(sample, now, maximum_sample_age) && valid_vehicle_sample(sample);
  bool any = false;
  for (const auto& d : reading_definitions)
    any |= bool(sample.*d.member);
  for (const auto& state : sample.indicators)
    any |= state.has_value();
  label(image, 20, 12, 1,
        !valid || !any     ? "DATA UNAVAILABLE"
        : sample.simulated ? "SIMULATION - NO CAR CONNECTED"
                           : "VEHICLE DATA",
        {125, 145, 165});
  const auto read = [&](const std::optional<double>& n) {
    return valid && n ? int(std::lround(*n)) : -1;
  };
  const int speed = read(sample.speed_kmh), rpm = read(sample.engine_rpm);
  number(image, 120, 38, 28, speed, 3, speed < 0 ? dim : pale);
  number(image, 590, 38, 28, rpm, 4, rpm < 0 ? dim : pale);
  label(image, 40, 108, 2, "KM/H", speed_color);
  label(image, 515, 108, 2, "RPM", rpm_color);
  rectangle(image, 475, 32, 2, 96, dim);
  rectangle(image, 40, 130, 400, 4, dim);
  rectangle(image, 515, 130, 405, 4, dim);
  if (speed >= 0) rectangle(image, 40, 130, 400 * speed / 300, 4, speed_color);
  if (rpm >= 0) rectangle(image, 515, 130, 405 * rpm / 9000, 4, rpm_color);
  for (std::size_t i = 2; i < reading_definitions.size(); ++i) {
    const auto& d = reading_definitions[i];
    const int col = int((i - 2) % 4), row = int((i - 2) / 4);
    const int x = 20 + col * 235, y = 148 + row * 45;
    rectangle(image, x, y, 220, 39, {16, 26, 38});
    label(image, x + 8, y + 5, 1, d.label, {125, 145, 165});
    const auto& n = sample.*d.member;
    char text[32] = "--";
    if (valid && n)
      std::snprintf(text, sizeof(text), d.key == "battery_voltage" ? "%.1f" : "%.0f", *n);
    label(image, x + 8, y + 18, 2, text, valid && n ? pale : Rgb{105, 120, 137});
    label(image, x + 177, y + 24, 1, d.unit, {125, 145, 165});
  }
  const bool phase = indicator_flash_visible(now);
  int visible = 0;
  for (const auto& d : indicator_definitions) {
    const auto state = valid ? sample.indicator(d.id) : std::nullopt;
    const bool lit =
        state && (*state == IndicatorState::on || (*state == IndicatorState::flashing && phase));
    if (!lit) continue;
    const int x = 16 + (visible % 8) * 117, y = 246 + (visible / 8) * 34;
    ++visible;
    const auto color = indicator_color(d.color);
    rectangle(image, x, y, 110, 28, {24, 34, 43});
    marker(image, x + 3, y + 6, d.id, color);
    label(image, x + 32, y + 10, 1, d.label, color);
  }
  if (width != 960 || height != 360) {
    Image output{width, height, std::vector<Rgb>(std::size_t(width) * height, background)};
    const double fit = std::min(double(width) / 960, double(height) / 360);
    const unsigned w = unsigned(960 * fit), h = unsigned(360 * fit), left = (width - w) / 2,
                   top = (height - h) / 2;
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x)
        output.pixels[std::size_t(y + top) * width + x + left] =
            image.at(std::min(959u, unsigned(x / fit)), std::min(359u, unsigned(y / fit)));
    return output;
  }
  return image;
}

void write_ppm(const Image& image, std::ostream& output) {
  output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (const auto pixel : image.pixels) {
    const char bytes[]{static_cast<char>(pixel.r), static_cast<char>(pixel.g),
                       static_cast<char>(pixel.b)};
    output.write(bytes, sizeof(bytes));
  }
  if (!output) throw std::runtime_error("cluster image write failed");
}

}  // namespace headunit
