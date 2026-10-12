// Standalone regression test: C++23, sdk/include on the include path, SSSE3 on x86-64.
#include <rex/audio/conversion.h>
#include <rex/audio/output_limiter.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
}

int main() {
  constexpr size_t frames = 256;
  rex::audio::StereoFold fold;
  fold.lfe = 0.5f;
  fold.scale = 0.70710678f;
  // Six planar, big-endian Xbox channels. Loud unequal stereo sums previously
  // clipped before limiting, losing both peak magnitude and stereo balance.
  const std::array<float, 6> channels{1.0f, 0.4f, 0.8f, 0.6f, 0.3f, 0.7f};
  std::array<float, frames * 6> input{};
  std::array<float, frames * 2> output{}, clamped{};
  for (size_t ch = 0; ch < 6; ++ch) {
    for (size_t i = 0; i < frames; ++i) input[ch * frames + i] = rex::byte_swap(channels[ch]);
  }
  rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
      output.data(), input.data(), frames, fold, 3.0f, false);
  const float mid = channels[2] * fold.center + channels[3] * fold.lfe;
  const float left = (channels[0] + mid + channels[4] * fold.surround) * fold.scale * 3.0f;
  const float right = (channels[1] + mid + channels[5] * fold.surround) * fold.scale * 3.0f;
  for (size_t i = 0; i < frames; ++i) {
    Check(Near(output[i * 2], left) && Near(output[i * 2 + 1], right),
          "The fold clipped peaks or decoded the channels incorrectly");
  }
  Check(left > 2.0f && right > 2.0f, "Regression input must exceed the previous 6 dB headroom");
  rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
      clamped.data(), input.data(), frames, fold, 3.0f);
  Check(clamped[0] == 1.0f && clamped[1] == 1.0f, "Default fold must keep the legacy clamp");
  float gain = 1.0f;
  rex::audio::LimitOutput(output.data(), frames, 2, 48000, gain);
  for (size_t i = 0; i < frames; ++i) {
    Check(Near(output[i * 2], 0.97f), "Peak exceeded the limiter ceiling");
    Check(Near(output[i * 2 + 1], 0.97f * right / left), "Limiter changed the stereo balance");
  }
  std::array<float, 4> quiet{0.2f, -0.3f, 0.1f, -0.1f};
  const auto original = quiet;
  gain = 1.0f;
  rex::audio::LimitOutput(quiet.data(), 2, 2, 48000, gain);
  Check(quiet == original && gain == 1.0f, "Quiet audio must pass without processing");
  // Recovery is defined in time, so 44.1 and 48 kHz movies behave alike.
  for (int rate : {44100, 48000}) {
    std::vector<float> silence(size_t(rate) * 2, 0.0f);
    float whole_gain = 0.4f, split_gain = whole_gain;
    rex::audio::LimitOutput(silence.data(), size_t(rate), 2, rate, whole_gain);
    for (size_t offset = 0; offset < size_t(rate); offset += frames) {
      const size_t count = std::min(frames, size_t(rate) - offset);
      rex::audio::LimitOutput(silence.data() + offset * 2, count, 2, rate, split_gain);
    }
    Check(whole_gain == split_gain && whole_gain > 0.999f,
          "Limiter recovery depends on block boundaries or fails to recover");
  }
  std::array<float, 4> invalid{std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(), 0.2f, -0.2f};
  gain = 1.0f;
  rex::audio::LimitOutput(invalid.data(), 2, 2, 48000, gain);
  Check(invalid[0] == 0.0f && invalid[1] == 0.0f && invalid[2] == 0.2f && gain == 1.0f,
        "Invalid samples poisoned subsequent audio");
  std::cout << "Audio output regression tests passed\n";
}
