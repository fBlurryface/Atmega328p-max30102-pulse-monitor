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
    NoFinger, Warming, Good, LowPulse, Unstable, Clipped, Uncorrelated, Range, NoBeat,
    HighVariation
  };
  explicit Measurement(uint32_t onIR = 5000, uint32_t offIR = 3500,
                       uint32_t minimumRed = 1000)
      : onIR_(onIR), offIR_(offIR), minimumRed_(minimumRed) { reset(); }
  void reset() {
    ticks_ = red_ = ir_ = 0;
    haveSample_ = finger_ = false;
    onCount_ = offCount_ = 0;
    resetAnalysis(Quality::NoFinger);
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
    // Two inexpensive high-pass stages suppress slow drift before peak finding.
    // Q8 filter and envelope states preserve fractional updates on small signals.
    dcIR_ += (int32_t(ir << 8) - dcIR_) / 32;
    const int32_t acIR = int32_t(ir) - (dcIR_ >> 8);
    trendIR_ += (acIR * 256 - trendIR_) / 32;
    smoothIR_ += (acIR * 256 - trendIR_ - smoothIR_) / 4;
    const int32_t signal = smoothIR_ / 256;
    envelope_ += (int32_t(absolute(signal) * 256UL) - envelope_) / 32;
    if (uint32_t(ticks_ - analysisStarted_) < 100) return;
    const int32_t minimum = int32_t(greater(ir / 2000, 10UL));
    const int32_t threshold = pulseThreshold();
    // A quiet portion of one cardiac cycle must not erase the interval history.
    if (envelope_ / 256 < minimum * 2 / 3) {
      if (lowCount_ < 100) ++lowCount_;
      if (lowCount_ == 100) {
        timingValid_ = spo2Valid_ = false;
        intervalCount_ = intervalWrite_ = 0;
        spo2Count_ = spo2Write_ = 0;
        quality_ = Quality::LowPulse;
      }
    } else lowCount_ = 0;
    // Track local prominence instead of requiring a negative-to-positive crossing.
    if (!candidate_) {
      if (signal < valley_) valley_ = signal;
      if (signal - valley_ >= threshold) {
        candidate_ = true; peak_ = signal; peakTick_ = ticks_; falls_ = 0;
      }
    }
    if (candidate_) {
      if (signal > peak_) {
        peak_ = signal; peakTick_ = ticks_; falls_ = 0;
      } else if (peak_ - signal >= threshold / 2) {
        if (++falls_ >= 2) {
          candidate_ = false; valley_ = signal;
          if (uint32_t(absolute(peak_)) < ir / 5) acceptPeak(peakTick_);
        }
      } else falls_ = 0;
    }
    if (havePeak_ && uint32_t(ticks_ - lastPeak_) > 280) {
      havePeak_ = timingValid_ = spo2Valid_ = false;
      intervalCount_ = intervalWrite_ = 0;
      spo2Count_ = spo2Write_ = 0; quality_ = Quality::NoBeat;
    }
    if (timingValid_ && !freshTiming()) {
      timingValid_ = spo2Valid_ = false;
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
  bool hrValid() const { return timingValid_ && freshTiming(); }
  bool spo2Valid() const { return spo2Valid_; }
  uint16_t bpm() const { return bpm_; }
  uint16_t spo2x10() const { return spo2_; }
  uint32_t red() const { return red_; }
  uint32_t ir() const { return ir_; }
  Quality quality() const { return quality_; }

 private:
  uint32_t onIR_, offIR_, minimumRed_;
  uint16_t redWindow_[100], irWindow_[100]; // 400 bytes, no intermediate arrays.
  uint16_t intervals_[4], cadence_[3], spo2History_[3];
  uint32_t ticks_, red_, ir_;
  uint32_t analysisStarted_, lastPeak_, peakTick_, lastCandidate_, lastGood_;
  uint32_t sumRed_, sumIR_;
  int32_t dcIR_, trendIR_, smoothIR_, envelope_, peak_, valley_;
  uint16_t bpm_, periodQ8_, spo2_, redCleanCount_;
  uint8_t onCount_, offCount_, write_, count_, sinceWindow_, decimation_;
  uint8_t intervalCount_, intervalWrite_, spo2Count_, spo2Write_, falls_, lowCount_;
  uint8_t fastCount_, cadenceCount_, cadenceWrite_;
  bool haveSample_, finger_, havePeak_, timingValid_, spo2Valid_, candidate_;
  bool beat_, pending_;
  Quality quality_;
  static uint32_t absolute(int32_t value) { return value < 0 ? uint32_t(-value) : uint32_t(value); }
  static uint32_t greater(uint32_t a, uint32_t b) { return a > b ? a : b; }
  int32_t pulseThreshold() const {
    const int32_t floor = int32_t(greater(ir_ / 2000, 10UL));
    const int32_t adaptive = (envelope_ / 256) * 3 / 2;
    return adaptive > floor ? adaptive : floor;
  }
  void resetAnalysis(Quality quality) {
    analysisStarted_ = ticks_;
    dcIR_ = int32_t(ir_ << 8);
    trendIR_ = smoothIR_ = envelope_ = peak_ = valley_ = 0;
    lastPeak_ = peakTick_ = sumRed_ = sumIR_ = 0;
    bpm_ = periodQ8_ = spo2_ = redCleanCount_ = 0;
    write_ = count_ = sinceWindow_ = decimation_ = 0;
    intervalCount_ = intervalWrite_ = spo2Count_ = spo2Write_ = falls_ = lowCount_ = 0;
    lastCandidate_ = lastGood_ = 0; fastCount_ = cadenceCount_ = cadenceWrite_ = 0;
    havePeak_ = timingValid_ = spo2Valid_ = candidate_ = beat_ = pending_ = false;
    quality_ = quality;
  }
  uint16_t medianInterval() const {
    if (!intervalCount_) return 0;
    uint16_t sorted[4];
    for (uint8_t i = 0; i < intervalCount_; ++i) {
      uint8_t j = i;
      while (j && sorted[j - 1] > intervals_[i]) { sorted[j] = sorted[j - 1]; --j; }
      sorted[j] = intervals_[i];
    }
    const uint8_t middle = intervalCount_ / 2;
    return intervalCount_ & 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
  }
  uint16_t averageInterval() const {
    const uint16_t center = medianInterval();
    uint16_t sum = 0; uint8_t n = 0;
    for (uint8_t i = 0; i < intervalCount_; ++i) {
      if (absolute(int32_t(intervals_[i]) - center) <= uint32_t(center / 4 + 3)) {
        sum += intervals_[i]; ++n;
      }
    }
    return n ? (sum + n / 2) / n : center;
  }
  bool freshTiming() const {
    const uint16_t period = averageInterval();
    const uint16_t timeout = period * 2 + 10 > 120 ? period * 2 + 10 : 120;
    return uint32_t(ticks_ - lastGood_) <= timeout;
  }
  void acceptPeak(uint32_t tick) {
    const uint32_t rawGap = tick - lastCandidate_;
    if (lastCandidate_ && rawGap >= 28 && rawGap <= 171) {
      cadence_[cadenceWrite_] = uint16_t(rawGap);
      cadenceWrite_ = (cadenceWrite_ + 1) % 3;
      if (cadenceCount_ < 3) ++cadenceCount_;
    } else cadenceCount_ = cadenceWrite_ = 0;
    if (lastCandidate_ && rawGap < 28) {
      if (fastCount_ < 3) ++fastCount_;
    } else fastCount_ = 0;
    lastCandidate_ = tick;
    if (fastCount_ >= 3) {
      timingValid_ = spo2Valid_ = false;
      intervalCount_ = intervalWrite_ = 0;
      spo2Count_ = spo2Write_ = 0; havePeak_ = false;
      quality_ = Quality::Unstable; return;
    }
    if (!havePeak_) { havePeak_ = true; lastPeak_ = tick; return; }
    uint32_t gap = tick - lastPeak_;
    const uint16_t previous = averageInterval();
    bool rebased = false;
    // Ignore a close secondary peak without moving the trusted timing anchor.
    if (gap < 28) { return; }
    if (intervalCount_ >= 3 && gap * 5 < uint32_t(previous) * 3) {
      // A sustained faster cadence must be able to replace the old rate.
      // Use three measured candidate intervals, not synthetic divided intervals.
      bool newCadence = cadenceCount_ == 3;
      if (newCadence) {
        const uint16_t center = (cadence_[0] + cadence_[1] + cadence_[2] + 1) / 3;
        for (uint8_t i = 0; i < 3; ++i)
          if (absolute(int32_t(cadence_[i]) - center) > uint32_t(center / 8 + 3)) newCadence = false;
      }
      if (!newCadence) { return; }
      for (uint8_t i = 0; i < 3; ++i) intervals_[i] = cadence_[i];
      intervalCount_ = 3; intervalWrite_ = 3; timingValid_ = false;
      spo2Valid_ = false; spo2Count_ = spo2Write_ = 0; rebased = true;
      gap = rawGap;
    }
    lastPeak_ = tick;
    if (gap > 171) {
      spo2Valid_ = false;
      spo2Count_ = spo2Write_ = 0;
      quality_ = Quality::NoBeat; return;
    }
    if (!rebased) {
      intervals_[intervalWrite_] = uint16_t(gap);
      intervalWrite_ = (intervalWrite_ + 1) & 3;
      if (intervalCount_ < 4) ++intervalCount_;
    }
    const uint16_t period = averageInterval();
    uint8_t coherent = 0;
    for (uint8_t i = 0; i < intervalCount_; ++i)
      if (absolute(int32_t(intervals_[i]) - period) <= uint32_t(period / 4 + 3)) ++coherent;
    const bool currentGood = absolute(int32_t(gap) - period) <= uint32_t(period / 4 + 3);
    if (currentGood && coherent >= 3) {
      const uint16_t target = uint16_t(uint32_t(period) * 256UL);
      if (!hrValid()) periodQ8_ = target;
      else periodQ8_ = uint16_t(int32_t(periodQ8_) + (int32_t(target) - periodQ8_) / 4);
      bpm_ = uint16_t((1536000UL + periodQ8_ / 2) / periodQ8_);
      timingValid_ = true; lastGood_ = ticks_;
    } else if (!currentGood) {
      spo2Valid_ = false;
      spo2Count_ = spo2Write_ = 0; quality_ = Quality::Unstable;
    }
    // Single-pulse feedback is independent of the SpO2 window's validity.
    if (currentGood && coherent >= 2) { beat_ = true; }
    if (hrValid() &&
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
      rejectWindow(Quality::LowPulse); return;
    }
    const float piI = sqrtf(i2 / 100.0f) / dcI;
    if (!(piI >= 0.001f && piI <= 0.10f)) {
      rejectWindow(piI > 0.10f ? Quality::HighVariation : Quality::LowPulse); return;
    }
    if (!hrValid()) { rejectWindow(Quality::NoBeat); return; }
    const float periodScore = periodicity(dcI, slopeI);
    if (!(periodScore >= 0.65f)) { rejectWindow(Quality::Unstable); return; }
    if (redClipped) { rejectWindow(Quality::Clipped); return; }
    if (redCleanCount_ < 400) { rejectWindow(Quality::Uncorrelated); return; }
    if (!(dcR >= minimumRed_ / 4.0f && r2 > 0)) { rejectWindow(Quality::Uncorrelated); return; }
    const float piR = sqrtf(r2 / 100.0f) / dcR;
    const float corr = cross / sqrtf(r2 * i2);
    if (fabsf(slopeI * 198.0f) > dcI * 0.10f || fabsf(slopeR * 198.0f) > dcR * 0.10f) {
      rejectWindow(Quality::Unstable); return;
    }
    if (!(piR >= 0.001f && piR <= 0.10f && corr >= 0.80f)) {
      rejectWindow(Quality::Uncorrelated); return;
    }
    const float ratio = piR / piI;
    if (!(ratio > 0.02f && ratio < 1.84f)) { rejectWindow(Quality::Range); return; }
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
