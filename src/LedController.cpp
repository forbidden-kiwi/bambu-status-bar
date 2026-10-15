#include "LedController.h"

#include <math.h>

namespace {
uint8_t brightnessToPwm(uint8_t percent) {
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255) / 100);
}
}  // namespace

bool LedController::addLedsForPin(uint8_t pin) {
#define LED_PIN_CASE(p)                              \
  case p:                                            \
    FastLED.addLeds<WS2812B, p, GRB>(leds_, count_); \
    return true
  switch (pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    LED_PIN_CASE(0);
    LED_PIN_CASE(1);
    LED_PIN_CASE(2);
    LED_PIN_CASE(3);
    LED_PIN_CASE(4);
    LED_PIN_CASE(5);
    LED_PIN_CASE(6);
    LED_PIN_CASE(7);
    LED_PIN_CASE(8);
    LED_PIN_CASE(9);
    LED_PIN_CASE(10);
    LED_PIN_CASE(11);
    LED_PIN_CASE(14);
    LED_PIN_CASE(15);
    LED_PIN_CASE(18);
    LED_PIN_CASE(19);
    LED_PIN_CASE(20);
    LED_PIN_CASE(21);
    LED_PIN_CASE(22);
    LED_PIN_CASE(23);
#elif defined(CONFIG_IDF_TARGET_ESP32)
    LED_PIN_CASE(0);
    LED_PIN_CASE(2);
    LED_PIN_CASE(4);
    LED_PIN_CASE(5);
    LED_PIN_CASE(12);
    LED_PIN_CASE(13);
    LED_PIN_CASE(14);
    LED_PIN_CASE(15);
    LED_PIN_CASE(16);
    LED_PIN_CASE(17);
    LED_PIN_CASE(18);
    LED_PIN_CASE(19);
    LED_PIN_CASE(21);
    LED_PIN_CASE(22);
    LED_PIN_CASE(23);
    LED_PIN_CASE(25);
    LED_PIN_CASE(26);
    LED_PIN_CASE(27);
    LED_PIN_CASE(32);
    LED_PIN_CASE(33);
#endif
    default:
      FastLED.addLeds<WS2812B, hw::kDefaultDataPin, GRB>(leds_, count_);
      return false;
  }
#undef LED_PIN_CASE
}

void LedController::begin(uint8_t dataPin, uint16_t ledCount) {
  count_ = ledCount < 1 ? 1 : ledCount;
  if (count_ > hw::kMaxNumLeds) count_ = hw::kMaxNumLeds;
  if (!hw::isSupportedLedDataPin(dataPin)) dataPin = hw::kDefaultDataPin;
  addLedsForPin(dataPin);
  FastLED.setMaxPowerInMilliWatts(hw::kMaxMilliwatts);
  FastLED.setBrightness(brightnessToPwm(style_.brightness));
  FastLED.clear(true);
}

void LedController::setStyle(const LedStyle& style) {
  style_ = style;
  if (style_.speed < 1) style_.speed = 1;
  if (style_.speed > 100) style_.speed = 100;
  if (style_.brightness > 100) style_.brightness = 100;
  auto clampSlot = [](LedStyleSlot& slot, LedEffect fallback) {
    slot.effect = clampLedEffect(static_cast<uint8_t>(slot.effect), fallback);
  };
  clampSlot(style_.idle, LedEffect::Pulse);
  clampSlot(style_.printing, LedEffect::Progress);
  clampSlot(style_.error, LedEffect::Strobe);
  clampSlot(style_.homing, LedEffect::Comet);
  clampSlot(style_.leveling, LedEffect::Comet);
  clampSlot(style_.preparing, LedEffect::Comet);
  clampSlot(style_.heating, LedEffect::Progress);
  clampSlot(style_.downloading, LedEffect::Comet);
  clampSlot(style_.paused, LedEffect::Pulse);
  clampSlot(style_.finished, LedEffect::Solid);
  FastLED.setBrightness(brightnessToPwm(style_.brightness));
}

uint16_t LedController::scaled(uint16_t baseMs) const {
  uint8_t speed = style_.speed < 1 ? 1 : style_.speed;
  uint32_t period = (static_cast<uint32_t>(baseMs) * 50) / speed;
  if (period < 16) period = 16;
  if (period > 20000) period = 20000;
  return static_cast<uint16_t>(period);
}

void LedController::showLeds() {
  if (style_.reverse) {
    for (int i = 0, j = static_cast<int>(count_) - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
  FastLED.show();
  // Restore logical order so stateful effects (sparkle/ember) keep working.
  if (style_.reverse) {
    for (int i = 0, j = static_cast<int>(count_) - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
}

void LedController::showBlack() {
  fill_solid(leds_, count_, CRGB::Black);
  showLeds();
}

void LedController::renderSolid(CRGB color) {
  fill_solid(leds_, count_, color);
  showLeds();
}

void LedController::renderProgressBar(CRGB onColor, CRGB offColor, uint8_t percent) {
  int onLeds = static_cast<int>(count_ * (percent / 100.0f) + 0.5f);
  for (int i = 0; i < static_cast<int>(count_); i++) {
    leds_[i] = (i < onLeds) ? onColor : offColor;
  }
  showLeds();
}

void LedController::renderHeating(uint8_t percent) {
  int lit = static_cast<int>(count_ * (percent / 100.0f) + 0.5f);
  fill_solid(leds_, count_, CRGB(30, 0, 0));
  for (int i = 0; i < lit; i++) {
    leds_[i] = CRGB(map(i, 0, max(lit - 1, 1), 64, 255), map(i, 0, max(lit - 1, 1), 21, 85), 0);
  }
  showLeds();
}

void LedController::renderComet(CRGB color, unsigned long nowMs, uint16_t stepMs) {
  // Soft ping-pong (zero velocity at ends) + fade trail so reverse never snaps the tail.
  if (count_ == 0) {
    showLeds();
    return;
  }
  if (stepMs < 8) stepMs = 8;

  if (count_ == 1) {
    leds_[0] = color;
    showLeds();
    return;
  }

  const float span = static_cast<float>(count_ - 1);
  // One full round trip ≈ same duration as the old linear bounce.
  const uint32_t periodMs = static_cast<uint32_t>(span * stepMs * 2.0f);
  const float phase = static_cast<float>(nowMs % periodMs) / static_cast<float>(periodMs);
  // 0 → span → 0 with cos ease: derivative is 0 at both ends.
  const float pos = span * 0.5f * (1.0f - cosf(phase * 2.0f * 3.14159265f));
  const bool atStart = pos <= 1.0f;
  const bool atEnd = pos >= span - 1.0f;

  // Persistence trail — skip outermost LEDs while the head dwells there so tips don't flicker.
  const uint8_t fade = static_cast<uint8_t>(18 + (100 - style_.speed) / 4);
  for (int i = 0; i < static_cast<int>(count_); i++) {
    if (atStart && i == 0) continue;
    if (atEnd && i == static_cast<int>(count_) - 1) continue;
    leds_[i].nscale8(255 - fade);
  }

  // Anti-aliased head — absolute write so fade never fights the tip brightness.
  const int i0 = static_cast<int>(pos);
  const float frac = pos - static_cast<float>(i0);
  auto plot = [&](int idx, float bri) {
    if (idx < 0 || idx >= static_cast<int>(count_) || bri <= 0.0f) return;
    if (bri > 1.0f) bri = 1.0f;
    CRGB c = color;
    c.nscale8(static_cast<uint8_t>(bri * 255.0f));
    leds_[idx] = c;
  };
  plot(i0, 1.0f - frac);
  plot(i0 + 1, frac);

  // Hold the outermost pixel solid for the whole turnaround dwell.
  if (atStart) leds_[0] = color;
  if (atEnd) leds_[static_cast<int>(count_) - 1] = color;
  showLeds();
}

void LedController::renderWave(unsigned long nowMs, uint8_t hue) {
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  // One sine period across the whole strip (single crest), independent of LED count.
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint8_t v = sin8(static_cast<uint8_t>((i * 256) / count_ + phase * 40));
    leds_[i] = CHSV(hue, 255, v);
  }
  showLeds();
}

void LedController::renderWaveColor(CRGB color, unsigned long nowMs) {
  // Scale the chosen RGB color — do not force HSV sat=255 (white → hue 0 = red).
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  // One sine period across the whole strip (single crest), independent of LED count.
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint8_t v = sin8(static_cast<uint8_t>((i * 256) / count_ + phase * 40));
    CRGB lit = color;
    lit.nscale8(v);
    leds_[i] = lit;
  }
  showLeds();
}

void LedController::renderPulse(CRGB color, unsigned long nowMs, uint16_t periodMs) {
  // Smooth sine breathe with a fixed dark hold at the bottom of each cycle.
  if (periodMs < 100) periodMs = 100;
  uint16_t darkHold = 250;
  if (darkHold > (periodMs * 3) / 10) darkHold = (periodMs * 3) / 10;
  const uint16_t breathMs = periodMs - darkHold;
  const uint16_t t = nowMs % periodMs;

  uint8_t scale = 0;
  if (t >= darkHold && breathMs > 0) {
    const float u = static_cast<float>(t - darkHold) / static_cast<float>(breathMs);
    // sin(pi*u): 0 → 1 → 0 with soft peak, no linear kink
    const float env = sinf(u * 3.14159265f);
    scale = static_cast<uint8_t>(env * 255.0f);
  }
  CRGB col = color;
  col.nscale8(scale);
  fill_solid(leds_, count_, col);
  showLeds();
}

void LedController::renderStrobe(CRGB color, unsigned long nowMs, uint16_t onMs, uint16_t offMs) {
  uint16_t t = nowMs % (onMs + offMs);
  fill_solid(leds_, count_, t < onMs ? color : CRGB::Black);
  showLeds();
}

void LedController::renderRainbow(unsigned long nowMs) {
  uint8_t speed = style_.speed < 1 ? 1 : style_.speed;
  uint8_t base = static_cast<uint8_t>((nowMs / (20 + (100 - speed))) & 0xFF);
  const int step = count_ > 0 ? (256 / count_) : 0;
  for (int i = 0; i < static_cast<int>(count_); i++) {
    leds_[i] = CHSV(static_cast<uint8_t>(base + i * step), 255, 255);
  }
  showLeds();
}

void LedController::renderSparkle(CRGB color, unsigned long nowMs) {
  static unsigned long lastStep = 0;
  fadeToBlackBy(leds_, count_, 40);
  if (nowMs - lastStep >= scaled(40)) {
    lastStep = nowMs;
    int sparks = 1 + (style_.speed / 40);
    for (int s = 0; s < sparks; s++) {
      leds_[random16(count_)] = color;
    }
  }
  showLeds();
}

void LedController::renderEmber(unsigned long nowMs) {
  (void)nowMs;
  // Horizontal glow: cool, blur left/right, spark along the strip (not "up").
  for (int i = 0; i < static_cast<int>(count_); i++) {
    heat_[i] = qsub8(heat_[i], random8(1, 10));
  }

  uint8_t next[hw::kMaxNumLeds];
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint16_t sum = static_cast<uint16_t>(heat_[i]) * 2;
    sum += (i > 0) ? heat_[i - 1] : heat_[i];
    sum += (i + 1 < static_cast<int>(count_)) ? heat_[i + 1] : heat_[i];
    next[i] = static_cast<uint8_t>(sum / 4);
  }
  for (int i = 0; i < static_cast<int>(count_); i++) {
    heat_[i] = next[i];
  }

  const uint8_t sparkChance = static_cast<uint8_t>(28 + style_.speed / 2);
  if (count_ > 0 && random8() < sparkChance) {
    const int x = random16(count_);
    heat_[x] = qadd8(heat_[x], random8(140, 255));
    if (x > 0) heat_[x - 1] = qadd8(heat_[x - 1], random8(30, 90));
    if (x + 1 < static_cast<int>(count_)) heat_[x + 1] = qadd8(heat_[x + 1], random8(30, 90));
  }

  for (int j = 0; j < static_cast<int>(count_); j++) {
    leds_[j] = HeatColor(heat_[j]);
  }
  showLeds();
}

void LedController::renderEffect(LedEffect effect, CRGB color, uint8_t progressPercent, unsigned long nowMs) {
  switch (effect) {
    case LedEffect::Solid:
      renderSolid(color);
      break;
    case LedEffect::Comet:
      renderComet(color, nowMs, scaled(70));
      break;
    case LedEffect::Wave:
      renderWaveColor(color, nowMs);
      break;
    case LedEffect::Strobe:
      renderStrobe(color, nowMs, scaled(180), scaled(180));
      break;
    case LedEffect::Progress:
      renderProgressBar(color, CRGB::Black, progressPercent);
      break;
    case LedEffect::Rainbow:
      renderRainbow(nowMs);
      break;
    case LedEffect::Sparkle:
      renderSparkle(color, nowMs);
      break;
    case LedEffect::Ember:
      renderEmber(nowMs);
      break;
    case LedEffect::Pulse:
    default:
      renderPulse(color, nowMs, scaled(3000));
      break;
  }
}

void LedController::renderSlot(const LedStyleSlot& slot, uint8_t progressPercent, unsigned long nowMs) {
  renderEffect(slot.effect, slot.color, progressPercent, nowMs);
}

void LedController::render(SystemState state, const PrinterTelemetry& telemetry, unsigned long nowMs) {
  FastLED.setBrightness(brightnessToPwm(style_.brightness));

  if (state == SystemState::Finished && telemetry.finishedAtMs != 0 &&
      nowMs - telemetry.finishedAtMs >= finishedHoldMs_) {
    state = SystemState::Idle;
  }

  // asleep_ covers standby idle and optional chamber-light follow (any state).
  if (asleep_) {
    showBlack();
    return;
  }

  switch (state) {
    case SystemState::Provisioning:
      renderComet(CRGB::Blue, nowMs, scaled(60));
      break;
    case SystemState::WifiConnecting:
      renderWave(nowMs, 160);
      break;
    case SystemState::MqttConnecting:
      renderStrobe(CRGB::Blue, nowMs, scaled(120), scaled(120));
      break;
    case SystemState::Idle:
      renderSlot(style_.idle, 100, nowMs);
      break;
    case SystemState::Homing:
      renderSlot(style_.homing, 100, nowMs);
      break;
    case SystemState::Leveling:
      renderSlot(style_.leveling, 100, nowMs);
      break;
    case SystemState::Preparing:
      renderSlot(style_.preparing, 100, nowMs);
      break;
    case SystemState::Downloading:
      renderSlot(style_.downloading, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Heating:
      // Keep the classic heat ramp when Progress is selected; other effects use the slot color.
      if (style_.heating.effect == LedEffect::Progress) {
        renderHeating(telemetry.progressPercent);
      } else {
        renderSlot(style_.heating, telemetry.progressPercent, nowMs);
      }
      break;
    case SystemState::Printing:
      renderSlot(style_.printing, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Paused:
      renderSlot(style_.paused, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Finished:
      renderSlot(style_.finished, 100, nowMs);
      break;
    case SystemState::Error:
      renderSlot(style_.error, 100, nowMs);
      break;
  }
}
