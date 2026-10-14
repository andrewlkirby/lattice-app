// The voice filter kernels, written once over a SIMD "traits" type V and compiled per instruction
// set: VoiceFilterSimd.cpp instantiates them for SSE2 (or plain floats), VoiceFilterSimdAvx2.cpp for
// AVX2 + FMA. Everything here is in an anonymous namespace, so each file gets its own copy and the
// linker never mixes them (docs/perf/avx2-plan.md, "Kernel files"). Include only after
// VoiceFilterKernels.h, and include nothing else here.
//
// V provides, for a register of V::width lanes:
//   reg, mask; width; chunk (samples per loadBlock / storeBlock)
//   set1, load / store (aligned float[width]), add, sub, mul, div, madd (a b + c), nmadd (c - a b),
//   max, abs, lessEqual, select (mask ? a : b), reciprocal (1/d to about 23 bits)
//   gather / scatter (one sample of each lane's buffer), loadBlock / storeBlock (chunk samples of
//   each lane, transposed so that out[s] holds sample i + s of every lane)
//
// Filters are serial chains (each sample needs the last), so one register of lanes runs at the
// latency of its chain, not the throughput of the core. Two registers interleaved (Paired<V>)
// keep twice as much independent work in flight; they are used whenever there are more lanes than
// one register holds.

#if defined(_MSC_VER) && !defined(__clang__)
#define LATTICE_KERNEL_INLINE __forceinline
#else
#define LATTICE_KERNEL_INLINE inline __attribute__((always_inline))
#endif

namespace lattice::dsp
{
namespace
{

constexpr int minOf(int a, int b) noexcept
{
    return a < b ? a : b;
}

/** Two registers of B side by side, as one register of twice the width. */
template <typename B>
struct Paired
{
    struct reg
    {
        typename B::reg a, b;
    };
    struct mask
    {
        typename B::mask a, b;
    };
    static constexpr int width = 2 * B::width;
    static constexpr int chunk = B::chunk;

    static LATTICE_KERNEL_INLINE reg set1(float v) noexcept
    {
        const auto r = B::set1(v);
        return {r, r};
    }
    static LATTICE_KERNEL_INLINE reg load(const float* p) noexcept { return {B::load(p), B::load(p + B::width)}; }
    static LATTICE_KERNEL_INLINE void store(float* p, reg v) noexcept
    {
        B::store(p, v.a);
        B::store(p + B::width, v.b);
    }
    static LATTICE_KERNEL_INLINE reg add(reg x, reg y) noexcept { return {B::add(x.a, y.a), B::add(x.b, y.b)}; }
    static LATTICE_KERNEL_INLINE reg sub(reg x, reg y) noexcept { return {B::sub(x.a, y.a), B::sub(x.b, y.b)}; }
    static LATTICE_KERNEL_INLINE reg mul(reg x, reg y) noexcept { return {B::mul(x.a, y.a), B::mul(x.b, y.b)}; }
    static LATTICE_KERNEL_INLINE reg div(reg x, reg y) noexcept { return {B::div(x.a, y.a), B::div(x.b, y.b)}; }
    static LATTICE_KERNEL_INLINE reg madd(reg x, reg y, reg z) noexcept
    {
        return {B::madd(x.a, y.a, z.a), B::madd(x.b, y.b, z.b)};
    }
    static LATTICE_KERNEL_INLINE reg nmadd(reg x, reg y, reg z) noexcept
    {
        return {B::nmadd(x.a, y.a, z.a), B::nmadd(x.b, y.b, z.b)};
    }
    static LATTICE_KERNEL_INLINE reg max(reg x, reg y) noexcept { return {B::max(x.a, y.a), B::max(x.b, y.b)}; }
    static LATTICE_KERNEL_INLINE reg abs(reg x) noexcept { return {B::abs(x.a), B::abs(x.b)}; }
    static LATTICE_KERNEL_INLINE reg reciprocal(reg x) noexcept { return {B::reciprocal(x.a), B::reciprocal(x.b)}; }
    static LATTICE_KERNEL_INLINE mask lessEqual(reg x, reg y) noexcept
    {
        return {B::lessEqual(x.a, y.a), B::lessEqual(x.b, y.b)};
    }
    static LATTICE_KERNEL_INLINE reg select(mask m, reg x, reg y) noexcept
    {
        return {B::select(m.a, x.a, y.a), B::select(m.b, x.b, y.b)};
    }
    static LATTICE_KERNEL_INLINE reg gather(float* const* buffers, int i) noexcept
    {
        return {B::gather(buffers, i), B::gather(buffers + B::width, i)};
    }
    static LATTICE_KERNEL_INLINE void scatter(float* const* buffers, int i, reg v) noexcept
    {
        B::scatter(buffers, i, v.a);
        B::scatter(buffers + B::width, i, v.b);
    }
    static LATTICE_KERNEL_INLINE void loadBlock(float* const* buffers, int i, reg* out) noexcept
    {
        typename B::reg a[chunk], b[chunk];
        B::loadBlock(buffers, i, a);
        B::loadBlock(buffers + B::width, i, b);
        for (int s = 0; s < chunk; ++s)
            out[s] = {a[s], b[s]};
    }
    static LATTICE_KERNEL_INLINE void storeBlock(float* const* buffers, int i, const reg* in) noexcept
    {
        typename B::reg a[chunk], b[chunk];
        for (int s = 0; s < chunk; ++s)
        {
            a[s] = in[s].a;
            b[s] = in[s].b;
        }
        B::storeBlock(buffers, i, a);
        B::storeBlock(buffers + B::width, i, b);
    }
};

/** One lane value per job (padding lanes repeat a real one). */
template <typename V, typename Job, typename Get>
LATTICE_KERNEL_INLINE typename V::reg fromLanes(const Job* const* lane, Get&& get) noexcept
{
    alignas(64) float t[V::width];
    for (int l = 0; l < V::width; ++l)
        t[l] = get(*lane[l]);
    return V::load(t);
}

/** Writes each real lane's value back. */
template <typename V, typename Job, typename Put>
LATTICE_KERNEL_INLINE void toLanes(typename V::reg v, const Job* const* lane, int real, Put&& put) noexcept
{
    alignas(64) float t[V::width];
    V::store(t, v);
    for (int l = 0; l < real; ++l)
        put(*lane[l], t[l]);
}

/** Runs lanes.process() over numSamples of every lane's buffer, setting the coefficients for the
    middle of each coefficient step, as the scalar filters do. */
template <typename V, typename Lanes>
void runLanes(Lanes& lanes, float* const* buffers, int numSamples) noexcept
{
    for (int i0 = 0; i0 < numSamples; i0 += voiceFilterCoefficientStep)
    {
        const int i1 = minOf(numSamples, i0 + voiceFilterCoefficientStep);
        lanes.setCoefficients(0.5f * static_cast<float>(i0 + i1));
        int i = i0;
        for (; i + V::chunk <= i1; i += V::chunk)
        {
            typename V::reg x[V::chunk];
            V::loadBlock(buffers, i, x);
            for (int s = 0; s < V::chunk; ++s)
                x[s] = lanes.process(x[s]);
            V::storeBlock(buffers, i, x);
        }
        for (; i < i1; ++i)
            V::scatter(buffers, i, lanes.process(V::gather(buffers, i)));
    }
}

/** Groups jobs into registers: two interleaved while more than one register's worth remain, then
    one. A register's padding lanes repeat its first job: they read that job's input and compute
    exactly its output (same values, same operations), so writing it again changes nothing, and
    their state is not written back. */
template <typename V, typename Job, typename Run>
void forEachRegister(const Job* jobs, int count, Run&& run) noexcept
{
    int done = 0;
    while (done < count)
    {
        const int left = count - done;
        if (left > V::width)
        {
            const int real = minOf(left, Paired<V>::width);
            run.template operator()<Paired<V>>(jobs + done, real);
            done += real;
        }
        else
        {
            run.template operator()<V>(jobs + done, left);
            done += left;
        }
    }
}

template <typename V, typename Job>
LATTICE_KERNEL_INLINE void laneJobs(const Job* jobs, int real, const Job** lane, float** buffers) noexcept
{
    for (int l = 0; l < V::width; ++l)
    {
        lane[l] = &jobs[l < real ? l : 0];
        buffers[l] = lane[l]->buffer;
    }
}

// --- SVF ----------------------------------------------------------------------------------------

constexpr float steepFirstK = 1.0f / 0.54119610f; // the 24 dB filter's first stage: Q = 0.541

template <typename V, SvfMode mode>
struct SvfLanes
{
    using R = typename V::reg;
    R ic1a, ic2a, ic1b, ic2b; // first stage (24 dB only), second stage
    R g0, gs, k2, gain;
    R a11, a12, a13, a21, a22, a23;
    bool steep;

    void load(const SvfJob* const* lane, bool isSteep, float inputGain) noexcept
    {
        steep = isSteep;
        ic1a = fromLanes<V>(lane, [](const SvfJob& j) { return j.first->ic1; });
        ic2a = fromLanes<V>(lane, [](const SvfJob& j) { return j.first->ic2; });
        ic1b = fromLanes<V>(lane, [](const SvfJob& j) { return j.second->ic1; });
        ic2b = fromLanes<V>(lane, [](const SvfJob& j) { return j.second->ic2; });
        g0 = fromLanes<V>(lane, [](const SvfJob& j) { return j.g; });
        gs = fromLanes<V>(lane, [](const SvfJob& j) { return j.gStep; });
        k2 = fromLanes<V>(lane, [](const SvfJob& j) { return j.k; });
        gain = V::set1(inputGain);
        a11 = a12 = a13 = V::set1(0.0f); // the first stage's, set per step when steep
    }

    void save(const SvfJob* const* lane, int real) const noexcept
    {
        toLanes<V>(ic1a, lane, real, [](const SvfJob& j, float v) { j.first->ic1 = v; });
        toLanes<V>(ic2a, lane, real, [](const SvfJob& j, float v) { j.first->ic2 = v; });
        toLanes<V>(ic1b, lane, real, [](const SvfJob& j, float v) { j.second->ic1 = v; });
        toLanes<V>(ic2b, lane, real, [](const SvfJob& j, float v) { j.second->ic2 = v; });
    }

    LATTICE_KERNEL_INLINE void setCoefficients(float t) noexcept
    {
        const R one = V::set1(1.0f);
        const R g = V::madd(gs, V::set1(t), g0);
        a21 = V::div(one, V::madd(g, V::add(g, k2), one));
        a22 = V::mul(g, a21);
        a23 = V::mul(g, a22);
        if (steep)
        {
            a11 = V::div(one, V::madd(g, V::add(g, V::set1(steepFirstK)), one));
            a12 = V::mul(g, a11);
            a13 = V::mul(g, a12);
        }
    }

    static LATTICE_KERNEL_INLINE R stage(R x, R& ic1, R& ic2, R a1, R a2, R a3, R k) noexcept
    {
        const R v3 = V::sub(x, ic2);
        const R v1 = V::madd(a1, ic1, V::mul(a2, v3));
        const R v2 = V::madd(a3, v3, V::madd(a2, ic1, ic2));
        ic1 = V::sub(V::add(v1, v1), ic1);
        ic2 = V::sub(V::add(v2, v2), ic2);
        if constexpr (mode == SvfMode::LowPass)
            return v2;
        else if constexpr (mode == SvfMode::HighPass)
            return V::sub(V::nmadd(k, v1, x), v2);
        else if constexpr (mode == SvfMode::BandPass)
            return v1;
        else
            return V::nmadd(k, v1, x);
    }

    LATTICE_KERNEL_INLINE R process(R x) noexcept
    {
        x = V::mul(x, gain);
        if (steep)
            x = stage(x, ic1a, ic2a, a11, a12, a13, V::set1(steepFirstK));
        return stage(x, ic1b, ic2b, a21, a22, a23, k2);
    }
};

template <typename V, SvfMode mode>
void svfKernel(const SvfJob* jobs, int count, int numSamples, bool steep, float inputGain) noexcept
{
    forEachRegister<V>(jobs, count, [&]<typename W>(const SvfJob* group, int real) {
        const SvfJob* lane[W::width];
        float* buffers[W::width];
        laneJobs<W>(group, real, lane, buffers);
        SvfLanes<W, mode> lanes;
        lanes.load(lane, steep, inputGain);
        runLanes<W>(lanes, buffers, numSamples);
        lanes.save(lane, real);
    });
}

// --- Ladder -------------------------------------------------------------------------------------

/** Sections: a fixed count (unrolled, no loop), or 0 for the count each chain has. */
template <typename V, int Sections>
struct AllpassLanes
{
    using R = typename V::reg;
    R c[allpassMaxSections], x[allpassMaxSections], y[allpassMaxSections];
    int n;

    LATTICE_KERNEL_INLINE int sections() const noexcept { return Sections > 0 ? Sections : n; }

    template <typename Chain>
    void load(const LadderJob* const* lane, Chain&& chain) noexcept
    {
        n = chain(*lane[0]).n;
        for (int i = 0; i < n; ++i)
        {
            c[i] = V::set1(chain(*lane[0]).c[i]); // one design for every voice
            x[i] = fromLanes<V>(lane, [&](const LadderJob& j) { return chain(j).x[i]; });
            y[i] = fromLanes<V>(lane, [&](const LadderJob& j) { return chain(j).y[i]; });
        }
    }

    template <typename Chain>
    void save(const LadderJob* const* lane, int real, Chain&& chain) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            toLanes<V>(x[i], lane, real, [&](const LadderJob& j, float v) { chain(j).x[i] = v; });
            toLanes<V>(y[i], lane, real, [&](const LadderJob& j, float v) { chain(j).y[i] = v; });
        }
    }

    LATTICE_KERNEL_INLINE R process(R in) noexcept
    {
        for (int i = 0; i < sections(); ++i)
        {
            const R out = V::madd(c[i], V::sub(in, y[i]), x[i]);
            x[i] = in;
            y[i] = out;
            in = out;
        }
        return in;
    }
};

template <typename V, int Sections>
struct LadderLanes
{
    using R = typename V::reg;
    R s1, s2, s3, s4, last, downDelay;
    AllpassLanes<V, Sections> upA, upB, downA, downB;
    R g0, gs, k, inGain, outGain;
    R G, G4, beta, bG, bG2, bG3; // per coefficient step

    void load(const LadderJob* const* lane, float inputGain, float outputScale) noexcept
    {
        s1 = fromLanes<V>(lane, [](const LadderJob& j) { return j.state->s1; });
        s2 = fromLanes<V>(lane, [](const LadderJob& j) { return j.state->s2; });
        s3 = fromLanes<V>(lane, [](const LadderJob& j) { return j.state->s3; });
        s4 = fromLanes<V>(lane, [](const LadderJob& j) { return j.state->s4; });
        last = fromLanes<V>(lane, [](const LadderJob& j) { return j.state->lastArgument; });
        downDelay = fromLanes<V>(lane, [](const LadderJob& j) { return j.oversampler->downDelay; });
        upA.load(lane, [](const LadderJob& j) -> AllpassState& { return j.oversampler->upA; });
        upB.load(lane, [](const LadderJob& j) -> AllpassState& { return j.oversampler->upB; });
        downA.load(lane, [](const LadderJob& j) -> AllpassState& { return j.oversampler->downA; });
        downB.load(lane, [](const LadderJob& j) -> AllpassState& { return j.oversampler->downB; });
        g0 = fromLanes<V>(lane, [](const LadderJob& j) { return j.g; });
        gs = fromLanes<V>(lane, [](const LadderJob& j) { return j.gStep; });
        k = fromLanes<V>(lane, [](const LadderJob& j) { return j.k; });
        inGain = V::set1(inputGain);
        // The level compensation, with the down-sampler's 1/2 folded in.
        outGain = V::mul(V::madd(V::set1(0.25f), k, V::set1(0.5f)), V::set1(outputScale));
    }

    void save(const LadderJob* const* lane, int real) const noexcept
    {
        toLanes<V>(s1, lane, real, [](const LadderJob& j, float v) { j.state->s1 = v; });
        toLanes<V>(s2, lane, real, [](const LadderJob& j, float v) { j.state->s2 = v; });
        toLanes<V>(s3, lane, real, [](const LadderJob& j, float v) { j.state->s3 = v; });
        toLanes<V>(s4, lane, real, [](const LadderJob& j, float v) { j.state->s4 = v; });
        toLanes<V>(last, lane, real, [](const LadderJob& j, float v) { j.state->lastArgument = v; });
        toLanes<V>(downDelay, lane, real, [](const LadderJob& j, float v) { j.oversampler->downDelay = v; });
        upA.save(lane, real, [](const LadderJob& j) -> AllpassState& { return j.oversampler->upA; });
        upB.save(lane, real, [](const LadderJob& j) -> AllpassState& { return j.oversampler->upB; });
        downA.save(lane, real, [](const LadderJob& j) -> AllpassState& { return j.oversampler->downA; });
        downB.save(lane, real, [](const LadderJob& j) -> AllpassState& { return j.oversampler->downB; });
    }

    LATTICE_KERNEL_INLINE void setCoefficients(float t) noexcept
    {
        const R one = V::set1(1.0f);
        const R g = V::madd(gs, V::set1(t), g0);
        beta = V::div(one, V::add(one, g));
        G = V::mul(g, beta);
        const R G2 = V::mul(G, G);
        G4 = V::mul(G2, G2);
        bG = V::mul(beta, G);
        bG2 = V::mul(beta, G2);
        bG3 = V::mul(bG2, G);
    }

    /** One step at the oversampled rate (LadderFilter::tick, without its divisions). */
    LATTICE_KERNEL_INLINE R tick(R u) noexcept
    {
        const R one = V::set1(1.0f);
        // tanh(a) / a for the fast tanh: (27 + a^2) / (27 + 9 a^2) up to |a| = 3, then 1 / |a|,
        // as one reciprocal of whichever denominator applies.
        const R a2 = V::mul(last, last);
        const R absA = V::abs(last);
        const auto inner = V::lessEqual(absA, V::set1(3.0f));
        const R num = V::select(inner, V::add(V::set1(27.0f), a2), one);
        const R den = V::select(inner, V::madd(V::set1(9.0f), a2, V::set1(27.0f)), V::max(absA, V::set1(1.0e-4f)));
        const R t = V::mul(num, V::reciprocal(den));

        // Solve the loop for the fourth stage's output, then run the four one-poles.
        const R S = V::add(V::madd(bG3, s1, V::mul(bG2, s2)), V::madd(bG, s3, V::mul(beta, s4)));
        const R G4t = V::mul(G4, t);
        const R y4 = V::mul(V::madd(G4t, u, S), V::reciprocal(V::madd(k, G4t, one)));
        const R argument = V::nmadd(k, y4, u);
        last = argument;
        const R x0 = V::mul(t, argument);

        R v = V::mul(V::sub(x0, s1), G);
        const R y1 = V::add(v, s1);
        s1 = V::add(y1, v);
        v = V::mul(V::sub(y1, s2), G);
        const R y2 = V::add(v, s2);
        s2 = V::add(y2, v);
        v = V::mul(V::sub(y2, s3), G);
        const R y3 = V::add(v, s3);
        s3 = V::add(y3, v);
        v = V::mul(V::sub(y3, s4), G);
        const R out = V::add(v, s4);
        s4 = V::add(out, v);
        return out;
    }

    LATTICE_KERNEL_INLINE R process(R x) noexcept
    {
        x = V::mul(x, inGain);
        const R ua = upA.process(x);
        const R ub = upB.process(x);
        const R ya = tick(ua);
        const R yb = tick(ub);
        const R da = downA.process(ya);
        const R db = downB.process(downDelay);
        downDelay = yb;
        return V::mul(V::add(da, db), outGain);
    }
};

template <typename V, int Sections>
void ladderRegisters(const LadderJob* jobs, int count, int numSamples, float inputGain, float outputScale) noexcept
{
    forEachRegister<V>(jobs, count, [&]<typename W>(const LadderJob* group, int real) {
        const LadderJob* lane[W::width];
        float* buffers[W::width];
        laneJobs<W>(group, real, lane, buffers);
        LadderLanes<W, Sections> lanes;
        lanes.load(lane, inputGain, outputScale);
        runLanes<W>(lanes, buffers, numSamples);
        lanes.save(lane, real);
    });
}

template <typename V>
void ladderKernel(const LadderJob* jobs, int count, int numSamples, float inputGain, float outputScale) noexcept
{
    if (count <= 0)
        return;
    // The voice design (HalfBand2x()) has three sections per branch: unrolled, with no loops.
    const HalfBandState& design = *jobs[0].oversampler;
    if (design.upA.n == 3 && design.upB.n == 3 && design.downA.n == 3 && design.downB.n == 3)
        ladderRegisters<V, 3>(jobs, count, numSamples, inputGain, outputScale);
    else
        ladderRegisters<V, 0>(jobs, count, numSamples, inputGain, outputScale);
}

/** The kernel table for traits V. */
template <typename V>
VoiceFilterKernels kernelsFor() noexcept
{
    return {&ladderKernel<V>,
            {&svfKernel<V, SvfMode::LowPass>, &svfKernel<V, SvfMode::HighPass>, &svfKernel<V, SvfMode::BandPass>,
             &svfKernel<V, SvfMode::Notch>}};
}

} // namespace
} // namespace lattice::dsp
