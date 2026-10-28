#pragma once
#include <Arduino.h>
#include <math.h>

// 100 Hz red/IR input, 25 Hz x 4 s compact rolling optical window.
// Original streaming detector; detrended RMS ratio and reference SpO2 polynomial
// adapted from Robert Fraczkiewicz's MAX30102_by_RF (MIT, full notice below).
// Reference calibration is NOT a calibration of this board's optical assembly.
class Measurement {
 public:
  enum class Quality : uint8_t {
    NoFinger, Warming, Good, LowPulse, Unstable, Clipped, Uncorrelated, Range, NoBeat
  };
  explicit Measurement(uint32_t onIR = 5000, uint32_t offIR = 3500,
                       uint32_t minimumRed = 1000)
      : onIR_(onIR), offIR_(offIR), minimumRed_(minimumRed) { reset(); }
  void reset() {
    ticks_ = red_ = ir_ = pulses_ = 0;
    haveSample_ = finger_ = false;
    onCount_ = offCount_ = 0;
    resetAnalysis(Quality::NoFinger);
  }
  bool setThresholds(uint32_t onIR, uint32_t offIR) {
    if (!offIR || offIR >= onIR || onIR >= 260000UL) return false;
    onIR_ = onIR; offIR_ = offIR;
    reset(); return true;
  }
  void push(uint32_t red, uint32_t ir) {
    const uint32_t previousRed = red_, previousIR = ir_;
    red_ = red; ir_ = ir; haveSample_ = true; ++ticks_;
    const bool clippedIR = ir >= 260000UL, clippedRed = red >= 260000UL;
    if (!finger_) {
      quality_ = clippedIR ? Quality::Clipped : Quality::NoFinger;
      if (!clippedIR && ir >= onIR_) {
        if (++onCount_ >= 20) {
          finger_ = true; onCount_ = offCount_ = 0;
          resetAnalysis(Quality::Warming);
        }
      } else onCount_ = 0;
      return;
    }
    if (ir < offIR_) {
      if (++offCount_ >= 30) {
        finger_ = false; onCount_ = offCount_ = 0;
        resetAnalysis(Quality::NoFinger); return;
      }
    } else offCount_ = 0;
    if (clippedIR) { resetAnalysis(Quality::Clipped); return; }
    // A large sample-to-sample step invalidates old results before rebuilding.
    if (absolute(int32_t(ir) - int32_t(previousIR)) > greater(ir / 8, 1000UL)) {
      resetAnalysis(Quality::Unstable); return;
    }
    if (absolute(int32_t(red) - int32_t(previousRed)) > greater(red / 8, 1000UL)) {
      write_ = count_ = sinceWindow_ = decimation_ = 0;
      sumRed_ = sumIR_ = 0; pending_ = false;
      spo2Valid_ = false; spo2Count_ = spo2Write_ = 0;
      redCleanCount_ = 0;
      quality_ = Quality::Uncorrelated;
    }
    if (clippedRed || red < minimumRed_) {
      spo2Valid_ = false; spo2Count_ = spo2Write_ = 0;
      redCleanCount_ = 0;
      quality_ = clippedRed ? Quality::Clipped : Quality::Uncorrelated;
    } else if (redCleanCount_ < 400) ++redCleanCount_;
    dcIR_ += (int32_t(ir << 8) - dcIR_) / 64;
    const int32_t acIR = int32_t(ir) - (dcIR_ >> 8);
    smoothIR_ += ((acIR * 256) - smoothIR_) / 4;
    const int32_t signal = smoothIR_ / 256;
    envelope_ += (int32_t(absolute(signal)) - envelope_) / 32;
    if (uint32_t(ticks_ - analysisStarted_) < 100) return;
    const int32_t minimum = int32_t(greater(ir / 1000, 8UL));
    const int32_t threshold = envelope_ * 3 / 5 > minimum ? envelope_ * 3 / 5 : minimum;
    if (absolute(signal) < uint32_t(minimum)) {
      if (lowCount_ < 50) ++lowCount_;
      if (lowCount_ == 50) {
        timingValid_ = spo2Valid_ = false;
        intervalCount_ = intervalWrite_ = 0;
        spo2Count_ = spo2Write_ = 0;
        quality_ = Quality::LowPulse;
      }
    } else lowCount_ = 0;
    if (signal < -threshold / 2 && !candidate_) armed_ = true;
    if (armed_ && !candidate_ && signal > threshold) {
      candidate_ = true; armed_ = false;
      peak_ = signal; peakTick_ = ticks_; falls_ = 0;
    }
    if (candidate_) {
      if (signal > peak_) {
        peak_ = signal; peakTick_ = ticks_; falls_ = 0;
      } else if (signal < peak_ - peak_ / 16) {
        if (++falls_ >= 3) {
          candidate_ = false;
          if (peak_ >= minimum && uint32_t(peak_) < ir / 5) acceptPeak(peakTick_);
        }
      } else falls_ = 0;
    }
    if (havePeak_ && uint32_t(ticks_ - lastPeak_) > 250) {
      havePeak_ = timingValid_ = spo2Valid_ = false;
      intervalCount_ = intervalWrite_ = 0;
      spo2Count_ = spo2Write_ = 0; quality_ = Quality::NoBeat;
    }
    sumRed_ += red; sumIR_ += ir;
    if (++decimation_ == 4) {
      redWindow_[write_] = uint16_t((sumRed_ / 4) >> 2);
      irWindow_[write_] = uint16_t((sumIR_ / 4) >> 2);
      write_ = (write_ + 1) % 100;
      if (count_ < 100) {
        if (++count_ == 100) { pending_ = true; sinceWindow_ = 0; }
      } else if (++sinceWindow_ >= 25) { pending_ = true; sinceWindow_ = 0; }
      sumRed_ = sumIR_ = 0; decimation_ = 0;
    }
  }
  // Call from loop(), never from an ISR. Work runs only once per new 1 s window.
  bool update() {
    if (!pending_) return false;
    pending_ = false;
    calculateWindow(); return true;
  }
  bool takeBeat() { const bool value = beat_; beat_ = false; return value; }
  bool hasSample() const { return haveSample_; }
  bool finger() const { return finger_; }
  bool hrValid() const { return timingValid_ && (!periodicKnown_ || periodicValid_); }
  bool spo2Valid() const { return spo2Valid_; }
  uint16_t bpm() const { return bpm_; }
  uint16_t spo2x10() const { return spo2_; }
  uint32_t red() const { return red_; }
  uint32_t ir() const { return ir_; }
  uint32_t samples() const { return ticks_; }
  uint32_t pulses() const { return pulses_; }
  uint32_t onThreshold() const { return onIR_; }
  uint32_t offThreshold() const { return offIR_; }
  uint16_t ratiox1000() const { return ratio_; }
  int16_t correlationx1000() const { return correlation_; }
  int16_t periodicityx1000() const { return periodicity_; }
  uint16_t perfusionx10000() const { return perfusion_; }
  uint8_t windowSamples() const { return count_; }
  Quality quality() const { return quality_; }

 private:
  uint32_t onIR_, offIR_, minimumRed_;
  uint16_t redWindow_[100], irWindow_[100]; // 400 bytes, no intermediate arrays.
  uint16_t intervals_[4], spo2History_[3];
  uint32_t ticks_, red_, ir_, pulses_, analysisStarted_, lastPeak_, peakTick_;
  uint32_t sumRed_, sumIR_;
  int32_t dcIR_, smoothIR_, envelope_, peak_;
  uint16_t bpm_, spo2_, ratio_, perfusion_, redCleanCount_;
  int16_t correlation_, periodicity_;
  uint8_t onCount_, offCount_, write_, count_, sinceWindow_, decimation_;
  uint8_t intervalCount_, intervalWrite_, spo2Count_, spo2Write_, falls_, lowCount_;
  bool haveSample_, finger_, havePeak_, timingValid_, spo2Valid_, armed_, candidate_;
  bool beat_, pending_, periodicKnown_, periodicValid_;
  Quality quality_;
  static uint32_t absolute(int32_t value) { return value < 0 ? uint32_t(-value) : uint32_t(value); }
  static uint32_t greater(uint32_t a, uint32_t b) { return a > b ? a : b; }
  void resetAnalysis(Quality quality) {
    analysisStarted_ = ticks_;
    dcIR_ = int32_t(ir_ << 8);
    smoothIR_ = envelope_ = peak_ = 0;
    lastPeak_ = peakTick_ = sumRed_ = sumIR_ = 0;
    bpm_ = spo2_ = ratio_ = perfusion_ = redCleanCount_ = 0; correlation_ = periodicity_ = 0;
    write_ = count_ = sinceWindow_ = decimation_ = 0;
    intervalCount_ = intervalWrite_ = spo2Count_ = spo2Write_ = falls_ = lowCount_ = 0;
    havePeak_ = timingValid_ = spo2Valid_ = armed_ = candidate_ = beat_ = pending_ = false;
    periodicKnown_ = periodicValid_ = false;
    quality_ = quality;
  }
  uint16_t averageInterval() const {
    if (!intervalCount_) return 0;
    uint16_t sum = 0;
    for (uint8_t i = 0; i < intervalCount_; ++i) sum += intervals_[i];
    return (sum + intervalCount_ / 2) / intervalCount_;
  }
  void acceptPeak(uint32_t tick) {
    if (!havePeak_) { havePeak_ = true; lastPeak_ = tick; return; }
    const uint32_t gap = tick - lastPeak_;
    if (gap < 28) { // Reject fast candidates without counting every other pulse.
      lastPeak_ = tick;
      intervalCount_ = intervalWrite_ = 0;
      spo2Count_ = spo2Write_ = 0;
      timingValid_ = spo2Valid_ = false; quality_ = Quality::Unstable;
      return;
    }
    lastPeak_ = tick;
    if (gap > 171) {
      intervalCount_ = intervalWrite_ = 0;
      spo2Count_ = spo2Write_ = 0; timingValid_ = spo2Valid_ = false;
      quality_ = Quality::NoBeat; return;
    }
    const uint16_t mean = averageInterval();
    if (intervalCount_ && absolute(int32_t(gap) - mean) > uint32_t(mean / 4 + 2)) {
      intervalCount_ = intervalWrite_ = 0;
      timingValid_ = spo2Valid_ = false;
      spo2Count_ = spo2Write_ = 0;
      quality_ = Quality::Unstable;
      return;
    }
    intervals_[intervalWrite_] = uint16_t(gap);
    intervalWrite_ = (intervalWrite_ + 1) & 3;
    if (intervalCount_ < 4) ++intervalCount_;
    timingValid_ = intervalCount_ >= 3;
    const uint16_t period = averageInterval();
    bpm_ = (6000UL + period / 2) / period;
    beat_ = !periodicKnown_ || periodicValid_;
    if (beat_) ++pulses_;
    if (timingValid_ && (!periodicKnown_ || periodicValid_) &&
        (quality_ == Quality::Warming || quality_ == Quality::LowPulse || quality_ == Quality::NoBeat))
      quality_ = Quality::Good;
  }
  uint16_t valueAt(const uint16_t *window, uint8_t index) const {
    return window[(write_ + index) % 100];
  }
  float periodicity(float dc, float slope) const {
    const int16_t center = int16_t((averageInterval() + 2) / 4);
    float best = -1.0f;
    for (int16_t lag = center - 1; lag <= center + 1; ++lag) {
      if (lag < 6 || lag > 44) continue;
      float cross = 0, a2 = 0, b2 = 0;
      for (uint8_t i = uint8_t(lag); i < 100; ++i) {
        const uint8_t j = i - lag;
        const float a = float(valueAt(irWindow_, i)) - dc - slope * (int16_t(2 * i) - 99);
        const float b = float(valueAt(irWindow_, j)) - dc - slope * (int16_t(2 * j) - 99);
        cross += a * b; a2 += a * a; b2 += b * b;
      }
      if (a2 > 0 && b2 > 0) {
        const float score = cross / sqrtf(a2 * b2);
        if (score > best) best = score;
      }
    }
    return best;
  }
  void rejectWindow(Quality quality) {
    spo2Valid_ = false; spo2Count_ = spo2Write_ = 0; quality_ = quality;
  }
  void calculateWindow() {
    uint32_t sumR = 0, sumI = 0;
    int32_t trendR = 0, trendI = 0;
    bool redClipped = false;
    correlation_ = 0; ratio_ = 0;
    for (uint8_t i = 0; i < 100; ++i) {
      const int16_t t = int16_t(2 * i) - 99;
      const uint16_t r = valueAt(redWindow_, i), ir = valueAt(irWindow_, i);
      if (r >= 65000) redClipped = true;
      sumR += r; sumI += ir;
      trendR += int32_t(t) * r; trendI += int32_t(t) * ir;
    }
    const float dcR = sumR / 100.0f, dcI = sumI / 100.0f;
    const float slopeR = trendR / 333300.0f, slopeI = trendI / 333300.0f;
    float r2 = 0, i2 = 0, cross = 0;
    for (uint8_t i = 0; i < 100; ++i) {
      const int16_t t = int16_t(2 * i) - 99;
      const float r = float(valueAt(redWindow_, i)) - dcR - slopeR * t;
      const float ir = float(valueAt(irWindow_, i)) - dcI - slopeI * t;
      r2 += r * r; i2 += ir * ir; cross += r * ir;
    }
    if (!(dcI > 0 && i2 > 0)) {
      periodicKnown_ = true; periodicValid_ = false;
      rejectWindow(Quality::LowPulse); return;
    }
    const float piI = sqrtf(i2 / 100.0f) / dcI;
    if (!(piI >= 0.001f && piI <= 0.10f)) {
      periodicKnown_ = true; periodicValid_ = false;
      rejectWindow(Quality::LowPulse); return;
    }
    perfusion_ = uint16_t(piI * 10000.0f);
    if (!timingValid_) { rejectWindow(Quality::NoBeat); return; }
    const float periodScore = periodicity(dcI, slopeI);
    periodicity_ = int16_t(periodScore * 1000.0f);
    periodicKnown_ = true; periodicValid_ = periodScore >= 0.65f;
    if (!periodicValid_) { rejectWindow(Quality::Unstable); return; }
    if (redClipped) { rejectWindow(Quality::Clipped); return; }
    if (redCleanCount_ < 400) { rejectWindow(Quality::Uncorrelated); return; }
    if (!(dcR >= minimumRed_ / 4.0f && r2 > 0)) { rejectWindow(Quality::Uncorrelated); return; }
    const float piR = sqrtf(r2 / 100.0f) / dcR;
    const float corr = cross / sqrtf(r2 * i2);
    correlation_ = int16_t(corr * 1000.0f);
    if (fabsf(slopeI * 198.0f) > dcI * 0.10f || fabsf(slopeR * 198.0f) > dcR * 0.10f) {
      rejectWindow(Quality::Unstable); return;
    }
    if (!(piR >= 0.001f && piR <= 0.10f && corr >= 0.80f)) {
      rejectWindow(Quality::Uncorrelated); return;
    }
    const float ratio = piR / piI;
    if (!(ratio > 0.02f && ratio < 1.84f)) { rejectWindow(Quality::Range); return; }
    ratio_ = uint16_t(ratio * 1000.0f);
    const float estimate = (-45.060f * ratio + 30.354f) * ratio + 94.845f;
    if (!(estimate >= 70.0f && estimate <= 100.0f)) {
      rejectWindow(Quality::Range); return;
    }
    spo2History_[spo2Write_] = uint16_t(estimate * 10.0f + 0.5f);
    spo2Write_ = (spo2Write_ + 1) % 3;
    if (spo2Count_ < 3) ++spo2Count_;
    uint16_t a = spo2History_[0];
    if (spo2Count_ == 1) spo2_ = a;
    else if (spo2Count_ == 2) spo2_ = (a + spo2History_[1] + 1) / 2;
    else {
      uint16_t b = spo2History_[1], c = spo2History_[2], tmp;
      if (a > b) { tmp = a; a = b; b = tmp; }
      if (b > c) { tmp = b; b = c; c = tmp; }
      if (a > b) b = a;
      spo2_ = b;
    }
    spo2Valid_ = true; quality_ = Quality::Good;
  }
};

/* Adapted reference method/polynomial license:
Copyright (C) 2017 Robert Fraczkiewicz, All Rights Reserved.
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL ROBERT
FRACZKIEWICZ BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
The mere transfer of this software does not imply any licenses of trade
secrets, proprietary technology, copyrights, patents, trademarks, maskwork
rights, or any other form of intellectual property whatsoever. Robert
Fraczkiewicz retains all ownership rights.
*/
