// =============================================================================
//  engine/processor.hpp — the native plugin contract (no VST, no CLAP, no JUCE).
//
//  Every EQ, compressor, artefact destroyer and limiter in the suite is an
//  IProcessor. The contract splits strictly by thread:
//
//    NON-RT (UI/loader thread)   prepare(), construction, destruction
//    RT     (audio/worker)       process(), reset(), latency(), param reads
//    ANY                         param(i)->set(), bypass flag
//
//  Each processor also publishes a static ProcessorInfo: parameter
//  descriptors AND a faceplate layout. The Graphics Engine draws faceplates
//  purely from this data, so adding a plugin never touches UI code.
// =============================================================================
#pragma once
#include "hc/core/aligned_buffer.hpp"
#include "hc/core/lockfree.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hc {

enum class Taper : std::uint8_t { Linear, Log, Decibel };

struct ParamDesc {
    std::string_view id, label, unit;
    float minValue, maxValue, defaultValue;
    Taper taper = Taper::Linear;
};

enum class ControlKind : std::uint8_t { Knob, Fader, Switch, GainReductionMeter, SpectrumView };

// Faceplate control placement in normalised [0,1] faceplate coordinates.
struct FaceplateControl {
    ControlKind kind;
    std::int16_t paramIndex;   // −1 for display-only widgets
    float x, y, size;
};

struct FaceplateDesc {
    float aspect = 2.0f;                 // width / height
    std::uint32_t accentRGBA = 0xC0FF3EFF; // HEM lime
    std::span<const FaceplateControl> controls;
};

struct ProcessorInfo {
    std::string_view id;        // stable, e.g. "hc.eq6"
    std::string_view name;
    std::string_view category;  // "EQ", "Dynamics", "Repair", "Master"
    std::uint32_t version = 1;
    std::span<const ParamDesc> params;
    FaceplateDesc faceplate;
};

class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual const ProcessorInfo& info() const noexcept = 0;

    // NON-RT: allocate everything here. maxBlock ≤ kMaxBlock.
    virtual void prepare(double sampleRate, std::uint32_t maxBlock) = 0;

    // RT.
    virtual void reset() noexcept = 0;
    virtual void process(AudioBlock& block) noexcept = 0;
    virtual std::uint32_t latency() const noexcept { return 0; }

    // Parameters (indices match info().params).
    virtual std::size_t numParams() const noexcept = 0;
    virtual AtomicParam& param(std::size_t i) noexcept = 0;

    void setBypassed(bool b) noexcept { bypass_.store(b, std::memory_order_relaxed); }
    bool bypassed() const noexcept { return bypass_.load(std::memory_order_relaxed); }

    // Optional single-value meter for faceplates (e.g. gain reduction dB).
    virtual float meterValue() const noexcept { return 0.0f; }

private:
    std::atomic<bool> bypass_{false};
};

// -----------------------------------------------------------------------------
// ProcessorChain — fixed-capacity serial chain. Built and prepared on the UI
// thread, then handed to the audio thread as a whole (see StemEngine).
// -----------------------------------------------------------------------------
class ProcessorChain {
public:
    bool add(std::unique_ptr<IProcessor> p) {
        if (count_ == kMaxChainSlots) return false;
        slots_[count_++] = std::move(p);
        return true;
    }
    void prepare(double sr, std::uint32_t maxBlock) {
        for (std::size_t i = 0; i < count_; ++i) slots_[i]->prepare(sr, maxBlock);
    }
    void process(AudioBlock& b) noexcept {
        for (std::size_t i = 0; i < count_; ++i) slots_[i]->process(b);
    }
    void reset() noexcept { for (std::size_t i = 0; i < count_; ++i) slots_[i]->reset(); }
    std::uint32_t latency() const noexcept {
        std::uint32_t l = 0;
        for (std::size_t i = 0; i < count_; ++i) l += slots_[i]->latency();
        return l;
    }
    std::size_t size() const noexcept { return count_; }
    IProcessor* at(std::size_t i) noexcept { return i < count_ ? slots_[i].get() : nullptr; }

private:
    std::unique_ptr<IProcessor> slots_[kMaxChainSlots];
    std::size_t count_ = 0;
};

// -----------------------------------------------------------------------------
// ProcessorRegistry — id → factory. Built-ins register explicitly at startup
// (no static-initialisation-order tricks).
// -----------------------------------------------------------------------------
class ProcessorRegistry {
public:
    using Factory = std::unique_ptr<IProcessor> (*)();
    struct Entry { std::string_view id; Factory make; };

    void add(std::string_view id, Factory f) { entries_.push_back({id, f}); }
    std::unique_ptr<IProcessor> create(std::string_view id) const {
        for (const auto& e : entries_) if (e.id == id) return e.make();
        return nullptr;
    }
    std::span<const Entry> entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

} // namespace hc
