#include "study_metrics.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace study {
using holo::json;
namespace {
json finite(double v){return std::isfinite(v)?json(v):json(nullptr);}
json ratioDb(double numerator,double denominator){return numerator>0&&denominator>0?finite(10*std::log10(numerator/denominator)):json(nullptr);}
struct Pchip {
  std::vector<double> x,y,d;
  Pchip(std::vector<double> xx,std::vector<double> yy):x(std::move(xx)),y(std::move(yy)),d(x.size()) {
    const size_t n=x.size();std::vector<double> h(n-1),s(n-1);
    for(size_t i=0;i+1<n;++i){h[i]=x[i+1]-x[i];if(h[i]<=0)throw std::runtime_error("Duplicate/non-increasing BD coordinate");s[i]=(y[i+1]-y[i])/h[i];}
    if(n==2){d[0]=d[1]=s[0];return;}
    auto edge=[](double h0,double h1,double s0,double s1){double v=((2*h0+h1)*s0-h0*s1)/(h0+h1);if(v*s0<=0)return 0.0;if(s0*s1<0&&std::abs(v)>3*std::abs(s0))return 3*s0;return v;};
    d[0]=edge(h[0],h[1],s[0],s[1]);d[n-1]=edge(h[n-2],h[n-3],s[n-2],s[n-3]);
    for(size_t i=1;i+1<n;++i)if(s[i-1]*s[i]>0){double w1=2*h[i]+h[i-1],w2=h[i]+2*h[i-1];d[i]=(w1+w2)/(w1/s[i-1]+w2/s[i]);}
  }
  double integral(double low,double high)const {
    double sum=0;
    for(size_t i=0;i+1<x.size();++i){double lo=std::max(low,x[i]),hi=std::min(high,x[i+1]);if(hi<=lo)continue;
      double h=x[i+1]-x[i],a=2*y[i]-2*y[i+1]+h*(d[i]+d[i+1]),b=-3*y[i]+3*y[i+1]-h*(2*d[i]+d[i+1]),c=h*d[i];
      auto F=[&](double u){return h*(a*u*u*u*u/4+b*u*u*u/3+c*u*u/2+y[i]*u);};sum+=F((hi-x[i])/h)-F((lo-x[i])/h);
    }return sum;
  }
};
}
ImageStats& ImageStats::operator+=(const ImageStats& b){samples+=b.samples;windows+=b.windows;clipped_reference+=b.clipped_reference;clipped_test+=b.clipped_test;sse+=b.sse;absolute_error+=b.absolute_error;reference_energy+=b.reference_energy;ssim_sum+=b.ssim_sum;return *this;}
json ImageStats::json(double peak)const {
  return {{"samples",samples},{"sse",sse},{"mse",samples?finite(sse/double(samples)):holo::json(nullptr)},
    {"rmse",samples?finite(std::sqrt(sse/double(samples))):holo::json(nullptr)},
    {"mae",samples?finite(absolute_error/double(samples)):holo::json(nullptr)},
    {"psnr_db",ratioDb(peak*peak*double(samples),sse)},{"snr_db",ratioDb(reference_energy,sse)},
    {"reference_energy",reference_energy},{"ssim",windows?finite(ssim_sum/double(windows)):holo::json(nullptr)},
    {"ssim_windows",windows},{"reference_clipped_fraction",samples?double(clipped_reference)/double(samples):0},
    {"test_clipped_fraction",samples?double(clipped_test)/double(samples):0},{"perfect_match",samples>0&&sse==0}};
}
ImageStats imageMetrics(const std::vector<float>& a,const std::vector<float>& b,unsigned w,unsigned h,double peak,bool ssim) {
  if(a.size()!=size_t(w)*h||a.size()!=b.size()||peak<=0)throw std::runtime_error("Invalid metric image");
  ImageStats out;out.samples=a.size();
  for(size_t i=0;i<a.size();++i){const double x=a[i],y=b[i];if(!std::isfinite(x)||!std::isfinite(y))throw std::runtime_error("Non-finite metric sample");double e=x-y;out.sse+=e*e;out.absolute_error+=std::abs(e);out.reference_energy+=x*x;}
  if(!ssim)return out;
  if(w<11||h<11)throw std::runtime_error("SSIM requires at least 11x11");
  // Single-scale Gaussian SSIM: 11x11, sigma=1.5, population covariance,
  // valid windows only, K1=.01/K2=.03; no input downsampling or prefiltering.
  std::array<double,11> kernel{};double normalization=0;
  for(int i=0;i<11;++i){kernel[i]=std::exp(-double((i-5)*(i-5))/(2*1.5*1.5));normalization+=kernel[i];}
  for(auto& v:kernel)v/=normalization;
  const unsigned validWidth=w-10;std::vector<std::array<double,5>> horizontal(size_t(validWidth)*h);
#pragma omp parallel for
  for(int row=0;row<int(h);++row)for(unsigned col=0;col<validWidth;++col){auto& m=horizontal[size_t(row)*validWidth+col];
    for(unsigned k=0;k<11;++k){const double x=a[size_t(row)*w+col+k],y=b[size_t(row)*w+col+k],g=kernel[k];m[0]+=g*x;m[1]+=g*y;m[2]+=g*x*x;m[3]+=g*y*y;m[4]+=g*x*y;}}
  const double c1=std::pow(.01*peak,2),c2=std::pow(.03*peak,2);double ssimSum=0;
#pragma omp parallel for reduction(+:ssimSum)
  for(int row=0;row<int(h)-10;++row)for(unsigned col=0;col<validWidth;++col){std::array<double,5> m{};
    for(unsigned k=0;k<11;++k)for(unsigned j=0;j<5;++j)m[j]+=kernel[k]*horizontal[(size_t(row)+k)*validWidth+col][j];
    const double vx=std::max(0.0,m[2]-m[0]*m[0]),vy=std::max(0.0,m[3]-m[1]*m[1]),cov=m[4]-m[0]*m[1];
    const double value=((2*m[0]*m[1]+c1)*(2*cov+c2))/((m[0]*m[0]+m[1]*m[1]+c1)*(vx+vy+c2));
    ssimSum+=std::max(-1.0,std::min(1.0,value));}
  out.ssim_sum=ssimSum;out.windows=uint64_t(validWidth)*(h-10);return out;
}
ComplexStats& ComplexStats::operator+=(const ComplexStats& b){samples+=b.samples;phase_samples+=b.phase_samples;real_sse+=b.real_sse;imag_sse+=b.imag_sse;energy+=b.energy;test_energy+=b.test_energy;normalized_sse+=b.normalized_sse;correlation_real+=b.correlation_real;correlation_imag+=b.correlation_imag;phase_sq+=b.phase_sq;phase_weight+=b.phase_weight;return *this;}
json ComplexStats::json()const {
  return {{"samples",samples},{"real_sse",real_sse},{"imag_sse",imag_sse},{"complex_sse",real_sse+imag_sse},
    {"complex_snr_db",ratioDb(energy,real_sse+imag_sse)},{"complex_nmse",energy>0?finite((real_sse+imag_sse)/energy):holo::json(nullptr)},
    {"real_imag_psnr_db",ratioDb(2*double(samples),normalized_sse)},
    {"complex_correlation_abs",energy>0&&test_energy>0?finite(std::hypot(correlation_real,correlation_imag)/std::sqrt(energy*test_energy)):holo::json(nullptr)},
    {"phase_weighted_rmse_rad",phase_weight>0?finite(std::sqrt(phase_sq/phase_weight)):holo::json(nullptr)},
    {"phase_valid_samples",phase_samples},{"reference_energy",energy},{"perfect_match",samples>0&&real_sse+imag_sse==0}};
}
ComplexStats complexMetrics(const std::vector<holo::Complex>& a,const std::vector<holo::Complex>& b,double range,double threshold) {
  if(a.size()!=b.size()||range<=0||threshold<0)throw std::runtime_error("Invalid complex metric input");
  ComplexStats out;out.samples=a.size();const double thresholdSq=threshold*threshold;
  for(size_t i=0;i<a.size();++i){const auto e=a[i]-b[i],cross=std::conj(a[i])*b[i];const double energy=std::norm(a[i]);
    out.real_sse+=e.real()*e.real();out.imag_sse+=e.imag()*e.imag();out.energy+=energy;out.test_energy+=std::norm(b[i]);out.correlation_real+=cross.real();out.correlation_imag+=cross.imag();
    if(energy>thresholdSq&&std::norm(b[i])>0){const double phase=std::arg(cross);out.phase_sq+=energy*phase*phase;out.phase_weight+=energy;++out.phase_samples;}}
  out.normalized_sse=(out.real_sse+out.imag_sse)/(range*range);return out;
}
json bdComparison(std::vector<Point> a,std::vector<Point> b,unsigned minimum) {
  json out={{"status","UNAVAILABLE"},{"interpolation","PCHIP; exact polynomial integral; no extrapolation"},{"baseline_points",a.size()},{"ours_points",b.size()}};
  if(a.size()<minimum||b.size()<minimum){out["reason"]="Insufficient finite points; at least four per curve required";return out;}
  auto prepare=[](std::vector<Point>& p){std::sort(p.begin(),p.end(),[](Point x,Point y){return x.rate<y.rate;});
    for(size_t i=0;i<p.size();++i)if(!std::isfinite(p[i].rate)||!std::isfinite(p[i].quality)||p[i].rate<=0||(i&&(p[i].rate<=p[i-1].rate||p[i].quality<=p[i-1].quality)))return false;return true;};
  if(!prepare(a)||!prepare(b)){out["reason"]="Non-monotone/duplicate rate or quality; retain raw points and revise QP list";return out;}
  std::vector<double> ar,aq,br,bq;for(auto p:a){ar.push_back(std::log(p.rate));aq.push_back(p.quality);}for(auto p:b){br.push_back(std::log(p.rate));bq.push_back(p.quality);}
  const double q0=std::max(aq.front(),bq.front()),q1=std::min(aq.back(),bq.back()),r0=std::max(ar.front(),br.front()),r1=std::min(ar.back(),br.back());
  out["common_quality_range"]={q0,q1};out["common_rate_range"]={std::exp(r0),std::exp(r1)};
  if(q1>q0){const double delta=(Pchip(bq,br).integral(q0,q1)-Pchip(aq,ar).integral(q0,q1))/(q1-q0);out["bd_rate_percent"]=finite(100*std::expm1(delta));}
  if(r1>r0)out["bd_quality_delta"]=(Pchip(br,bq).integral(r0,r1)-Pchip(ar,aq).integral(r0,r1))/(r1-r0);
  out["status"]=q1>q0&&r1>r0?"VALID":"PARTIAL_OR_NO_OVERLAP";
  if(q1<=q0||r1<=r0)out["reason"]="No common quality or rate interval; add QPs instead of extrapolating";
  return out;
}
}
