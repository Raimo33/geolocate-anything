// FFTW wrappers. Conventions follow numpy: rfft is unnormalised, backward transforms
// are unnormalised sums (callers scale).
#pragma once

#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace geo::fft {

using cd = std::complex<double>;
using cf = std::complex<float>;

// Aligned buffer from fftw_malloc.
template <class T>
class Buf {
 public:
  Buf() = default;
  explicit Buf(std::size_t n);
  Buf(Buf&& o) noexcept : p_(o.p_), n_(o.n_) {
    o.p_ = nullptr;
    o.n_ = 0;
  }
  Buf& operator=(Buf&& o) noexcept {
    std::swap(p_, o.p_);
    std::swap(n_, o.n_);
    return *this;
  }
  ~Buf();
  T* data() { return p_; }
  const T* data() const { return p_; }
  std::size_t size() const { return n_; }
  T& operator[](std::size_t i) { return p_[i]; }
  const T& operator[](std::size_t i) const { return p_[i]; }
  std::span<T> span() { return {p_, n_}; }
  std::span<const T> span() const { return {p_, n_}; }
  T* begin() { return p_; }
  T* end() { return p_ + n_; }

 private:
  T* p_ = nullptr;
  std::size_t n_ = 0;
};

// Real FFT of any length (numpy.fft.rfft: n/2 + 1 bins), computed in place: fill(dst)
// writes the n real inputs straight into the output buffer, so no input copy is held
// next to the spectrum.
Buf<cd> rfft_inplace(std::size_t n, const std::function<void(double*)>& fill);
// One-off unnormalised backward complex FFT in place: a[n] = sum_k a[k] e^{+2 pi i k n / M}.
void backward_inplace(std::span<cd> a);

// Reusable real FFT plans of a fixed size (double), for repeated transforms.
class RealPlan {
 public:
  explicit RealPlan(int n);
  ~RealPlan();
  RealPlan(const RealPlan&) = delete;
  RealPlan& operator=(const RealPlan&) = delete;
  int n() const { return n_; }
  // in: n reals (aligned Buf), out: n/2+1 complex (aligned)
  void forward(double* in, cd* out) const;

 private:
  int n_;
  void* fwd_;
};

// Float r2c/c2r pair of size n (power of two in practice), planned with FFTW_MEASURE and
// cached per size for the process; wisdom is kept in the cache directory.
class RealPlanF {
 public:
  static const RealPlanF& get(int n);
  int n() const { return n_; }
  void forward(float* in, cf* out) const;
  void inverse(cf* in, float* out) const;  // unnormalised; destroys in
  ~RealPlanF();

 private:
  explicit RealPlanF(int n);
  int n_;
  void* fwd_;
  void* inv_;
};

// Same, double precision (exact masked correlation).
class RealPlanD {
 public:
  static const RealPlanD& get(int n);
  int n() const { return n_; }
  void forward(double* in, cd* out) const;
  void inverse(cd* in, double* out) const;
  ~RealPlanD();

 private:
  explicit RealPlanD(int n);
  int n_;
  void* fwd_;
  void* inv_;
};

void save_wisdom();

}  // namespace geo::fft
