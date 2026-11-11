#include "dsp/fft.hpp"

#include <fftw3.h>

#include <cstring>
#include <map>
#include <stdexcept>

#include "store/store.hpp"

namespace geo::fft {

template <class T>
Buf<T>::Buf(std::size_t n) : p_(static_cast<T*>(fftw_malloc(sizeof(T) * (n ? n : 1)))), n_(n) {
  if (!p_) throw std::bad_alloc();
}
template <class T>
Buf<T>::~Buf() {
  if (p_) fftw_free(p_);
}
template class Buf<double>;
template class Buf<float>;
template class Buf<cd>;
template class Buf<cf>;

namespace {
bool g_wisdom_loaded = false, g_wisdom_dirty = false;
std::string wisdom_path(char kind) { return (store::cache_dir() / (std::string("fftw-wisdom-") + kind)).string(); }
void load_wisdom() {
  if (g_wisdom_loaded) return;
  g_wisdom_loaded = true;
  fftw_import_wisdom_from_filename(wisdom_path('d').c_str());
  fftwf_import_wisdom_from_filename(wisdom_path('f').c_str());
}
}  // namespace

void save_wisdom() {
  if (!g_wisdom_dirty) return;
  fftw_export_wisdom_to_filename(wisdom_path('d').c_str());
  fftwf_export_wisdom_to_filename(wisdom_path('f').c_str());
  g_wisdom_dirty = false;
}

Buf<cd> rfft_inplace(std::size_t n, const std::function<void(double*)>& fill) {
  Buf<cd> out(n / 2 + 1);  // 2 (n/2 + 1) >= n doubles: room for the real input
  auto* d = reinterpret_cast<double*>(out.data());
  fftw_plan p = fftw_plan_dft_r2c_1d(static_cast<int>(n), d, reinterpret_cast<fftw_complex*>(out.data()), FFTW_ESTIMATE);
  fill(d);
  fftw_execute(p);
  fftw_destroy_plan(p);
  return out;
}

void backward_inplace(std::span<cd> a) {
  if (a.empty()) return;
  Buf<cd> buf(a.size());
  auto* io = reinterpret_cast<fftw_complex*>(buf.data());
  fftw_plan p = fftw_plan_dft_1d(static_cast<int>(a.size()), io, io, FFTW_BACKWARD, FFTW_ESTIMATE);
  std::memcpy(buf.data(), a.data(), a.size() * sizeof(cd));
  fftw_execute(p);
  fftw_destroy_plan(p);
  std::memcpy(a.data(), buf.data(), a.size() * sizeof(cd));
}

RealPlan::RealPlan(int n) : n_(n) {
  Buf<double> in(static_cast<std::size_t>(n));
  Buf<cd> out(static_cast<std::size_t>(n / 2 + 1));
  fwd_ = fftw_plan_dft_r2c_1d(n, in.data(), reinterpret_cast<fftw_complex*>(out.data()), FFTW_ESTIMATE);
}
RealPlan::~RealPlan() { fftw_destroy_plan(static_cast<fftw_plan>(fwd_)); }
void RealPlan::forward(double* in, cd* out) const {
  fftw_execute_dft_r2c(static_cast<fftw_plan>(fwd_), in, reinterpret_cast<fftw_complex*>(out));
}

RealPlanF::RealPlanF(int n) : n_(n) {
  load_wisdom();
  Buf<float> r(static_cast<std::size_t>(n));
  Buf<cf> c(static_cast<std::size_t>(n / 2 + 1));
  fwd_ = fftwf_plan_dft_r2c_1d(n, r.data(), reinterpret_cast<fftwf_complex*>(c.data()), FFTW_MEASURE);
  inv_ = fftwf_plan_dft_c2r_1d(n, reinterpret_cast<fftwf_complex*>(c.data()), r.data(), FFTW_MEASURE);
  g_wisdom_dirty = true;
}
RealPlanF::~RealPlanF() {
  fftwf_destroy_plan(static_cast<fftwf_plan>(fwd_));
  fftwf_destroy_plan(static_cast<fftwf_plan>(inv_));
}
const RealPlanF& RealPlanF::get(int n) {
  static std::map<int, std::unique_ptr<RealPlanF>> plans;
  auto& p = plans[n];
  if (!p) p.reset(new RealPlanF(n));
  return *p;
}
void RealPlanF::forward(float* in, cf* out) const {
  fftwf_execute_dft_r2c(static_cast<fftwf_plan>(fwd_), in, reinterpret_cast<fftwf_complex*>(out));
}
void RealPlanF::inverse(cf* in, float* out) const {
  fftwf_execute_dft_c2r(static_cast<fftwf_plan>(inv_), reinterpret_cast<fftwf_complex*>(in), out);
}

RealPlanD::RealPlanD(int n) : n_(n) {
  load_wisdom();
  Buf<double> r(static_cast<std::size_t>(n));
  Buf<cd> c(static_cast<std::size_t>(n / 2 + 1));
  fwd_ = fftw_plan_dft_r2c_1d(n, r.data(), reinterpret_cast<fftw_complex*>(c.data()), FFTW_MEASURE);
  inv_ = fftw_plan_dft_c2r_1d(n, reinterpret_cast<fftw_complex*>(c.data()), r.data(), FFTW_MEASURE);
  g_wisdom_dirty = true;
}
RealPlanD::~RealPlanD() {
  fftw_destroy_plan(static_cast<fftw_plan>(fwd_));
  fftw_destroy_plan(static_cast<fftw_plan>(inv_));
}
const RealPlanD& RealPlanD::get(int n) {
  static std::map<int, std::unique_ptr<RealPlanD>> plans;
  auto& p = plans[n];
  if (!p) p.reset(new RealPlanD(n));
  return *p;
}
void RealPlanD::forward(double* in, cd* out) const {
  fftw_execute_dft_r2c(static_cast<fftw_plan>(fwd_), in, reinterpret_cast<fftw_complex*>(out));
}
void RealPlanD::inverse(cd* in, double* out) const {
  fftw_execute_dft_c2r(static_cast<fftw_plan>(inv_), reinterpret_cast<fftw_complex*>(in), out);
}

}  // namespace geo::fft
