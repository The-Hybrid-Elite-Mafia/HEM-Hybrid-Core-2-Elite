// =============================================================================
//  dsp/biquad.hpp — second-order IIR sections (EQ, K-weighting, crossovers).
//
//  Coefficients: Robert Bristow-Johnson "Audio EQ Cookbook" bilinear designs.
//  Structure: Transposed Direct Form II in DOUBLE precision.
//    y  = b0·x + s1
//    s1 = b1·x − a1·y + s2
//    s2 = b2·x − a2·y
//  Why double: a 30 Hz low shelf at 96 kHz puts poles within ~1e-3 of the unit
//  circle; single-precision coefficient quantisation there audibly shifts the
//  corner and raises the noise floor. 13 stems × a handful of bands in double
//  is a rounding error of CPU on any AVX2 machine.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hc {

struct BiquadCoeffs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

enum class FilterType : std::uint8_t { Bell, LowShelf, HighShelf, LowPass, HighPass, BandPass, Notch };

inline BiquadCoeffs designBiquad(FilterType type, double fs, double f0, double q, double gainDb) noexcept {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * std::fmin(f0, 0.49 * fs) / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case FilterType::Bell:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case FilterType::LowShelf: {
        const double sA = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sA);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sA);
        a0 = (A + 1) + (A - 1) * cw + sA;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sA;
        break;
    }
    case FilterType::HighShelf: {
        const double sA = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sA);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sA);
        a0 = (A + 1) - (A - 1) * cw + sA;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sA;
        break;
    }
    case FilterType::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::HighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::BandPass:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::Notch:
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// One section, one channel.
class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& c) noexcept { c_ = c; }
    const BiquadCoeffs& coeffs() const noexcept { return c_; }
    void reset() noexcept { s1_ = s2_ = 0.0; }

    double tick(double x) noexcept {
        const double y = c_.b0 * x + s1_;
        s1_ = c_.b1 * x - c_.a1 * y + s2_;
        s2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }
    void process(float* io, std::uint32_t n) noexcept {
        double s1 = s1_, s2 = s2_;
        const BiquadCoeffs c = c_;
        for (std::uint32_t i = 0; i < n; ++i) {
            const double x = io[i];
            const double y = c.b0 * x + s1;
            s1 = c.b1 * x - c.a1 * y + s2;
            s2 = c.b2 * x - c.a2 * y;
            io[i] = static_cast<float>(y);
        }
        s1_ = s1; s2_ = s2;
    }

private:
    BiquadCoeffs c_;
    double s1_ = 0.0, s2_ = 0.0;
};

} // namespace hc
