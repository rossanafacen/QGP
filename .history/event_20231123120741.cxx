// TRENTO: Reduced Thickness Event-by-event Nuclear Topology
// Copyright 2015 Jonah E. Bernhard, J. Scott Moreland
// MIT License

#include "event.h"
#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>


#include <boost/program_options/variables_map.hpp>

#include "nucleus.h"

#include "common.h"

namespace trento {

namespace {

constexpr double TINY = 1e-12;

// Generalized mean for p > 0.
// M_p(a, b) = (1/2*(a^p + b^p))^(1/p)
inline double positive_pmean(double p, double a, double b) {
  return std::pow(.5*(std::pow(a, p) + std::pow(b, p)), 1./p);
}

// Generalized mean for p < 0.
// Same as the positive version, except prevents division by zero.
inline double negative_pmean(double p, double a, double b) {
  if (a < TINY || b < TINY)
    return 0.;
  return positive_pmean(p, a, b);
}

// Generalized mean for p == 0.
inline double geometric_mean(double a, double b) {
//return std::pow(a*b,3./8.);
  return std::sqrt(a*b);
}

}  // unnamed namespace

// Determine the grid parameters like so:
//   1. Read and set step size from the configuration.
//   2. Read grid max from the config, then set the number of steps as
//      nsteps = ceil(2*max/step).
//   3. Set the actual grid max as max = nsteps*step/2.  Hence if the step size
//      does not evenly divide the config max, the actual max will be marginally
//      larger (by at most one step size).
Event::Event(const VarMap& var_map)
    : norm_(var_map["normalization"].as<double>()),
      dxy_(var_map["grid-step"].as<double>()),
      nsteps_(std::ceil(2.*var_map["grid-max"].as<double>()/dxy_)),
      xymax_(.5*nsteps_*dxy_),
      TA_(boost::extents[nsteps_][nsteps_]),
      TB_(boost::extents[nsteps_][nsteps_]),
      TA_det_(boost::extents[nsteps_][nsteps_]), 
      TB_det_(boost::extents[nsteps_][nsteps_]),
      TR_(boost::extents[nsteps_][nsteps_]),
      TAB_(boost::extents[nsteps_][nsteps_]),
      with_ncoll_(var_map["ncoll"].as<bool>()),
      EOS_() {
  // Choose which version of the generalized mean to use based on the
  // configuration.  The possibilities are defined above.  See the header for
  // more information.
  auto p = var_map["reduced-thickness"].as<double>();

  if (std::fabs(p) < TINY) {
    compute_reduced_thickness_ = [this]() {
      compute_reduced_thickness(geometric_mean);
    };
  } else if (p > 0.) {
    compute_reduced_thickness_ = [this, p]() {
      compute_reduced_thickness(
        [p](double a, double b) { return positive_pmean(p, a, b); });
    };
  } else {
    compute_reduced_thickness_ = [this, p]() {
      compute_reduced_thickness(
        [p](double a, double b) { return negative_pmean(p, a, b); });
    };
  }
}

void Event::compute(const Nucleus& nucleusA, const Nucleus& nucleusB,
                    const NucleonCommon& nucleon_common) {
  // Reset npart; compute_nuclear_thickness() increments it.
  npart_ = 0;
  compute_nuclear_thickness(nucleusA, nucleon_common, TA_);
  compute_nuclear_thickness(nucleusB, nucleon_common, TB_);
  compute_reduced_thickness_();
  //compute_nuclear_deterministic_thickness(nucleusA, nucleon_common, TA_det_);
  //compute_nuclear_deterministic_thickness(nucleusB, nucleon_common, TB_det_);
  //compute_ncoll();
  //accumulate_TAB(nucleusA, nucleusB, nucleon_common);
  compute_observables();
}

namespace {

// Limit a value to a range.
// Used below to constrain grid indices.
template <typename T>
inline const T& clip(const T& value, const T& min, const T& max) {
  if (value < min)
    return min;
  if (value > max)
    return max;
  return value;
}

}  // unnamed namespace

void Event::clear_TAB(void){
  ncoll_ = 0;
  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      TAB_[iy][ix] = 1.;
    }
  }
}



// WK: accumulate a Tpp to Ncoll density table
void Event::accumulate_TAB(NucleonData& A, NucleonData& B, NucleonCommon& nucleon_common){
  ncoll_ ++;

	// the loaction of A and B nucleon
	double xA = A.x() + xymax_, yA = A.y() + xymax_;
	double xB = B.x() + xymax_, yB = B.y() + xymax_;
	// the mid point of A and B
	double x = (xA+xB)/2.;
  double y = (yA+yB)/2.;
	
  //this is only useful to define the boundaries of each grid
  const double r = nucleon_common.max_impact();
	int ixmin = clip(static_cast<int>((x-r)/dxy_), 0, nsteps_-1);
  int iymin = clip(static_cast<int>((y-r)/dxy_), 0, nsteps_-1);
  int ixmax = clip(static_cast<int>((x+r)/dxy_), 0, nsteps_-1);
  int iymax = clip(static_cast<int>((y+r)/dxy_), 0, nsteps_-1);

  
    for (auto iy = iymin; iy <= iymax; ++iy) {
      double y_nucleus = (static_cast<double>(iy)+.5)*dxy_;
	    
      for (auto ix = ixmin; ix <= ixmax; ++ix) {
        double x_nucleus = (static_cast<double>(ix)+.5)*dxy_;
	      //TAB_[iy][ix] += nucleon_common.deterministic_thickness(A, x_nucleus, y_nucleus)
        //       * nucleon_common.deterministic_thickness(B, x_nucleus, y_nucleus);
        
        TAB_[iy][ix] += nucleon_common.thickness(
          nucleon, (ix+.5)*dxy_ - xymax_, (iy+.5)*dxy_ - xymax_
        )
        //TAB_[iy][ix] += 1;
      }
    }
}


void Event::compute_nuclear_thickness(
    const Nucleus& nucleus, const NucleonCommon& nucleon_common, Grid& TX) {
  // Construct the thickness grid by looping over participants and adding each
  // to a small subgrid within its radius.  Compared to the other possibility
  // (grid cells as the outer loop and participants as the inner loop), this
  // reduces the number of required distance-squared calculations by a factor of
  // ~20 (depending on the nucleon size).  The Event unit test verifies that the
  // two methods agree.

  // Wipe grid with zeros.
  std::fill(TX.origin(), TX.origin() + TX.num_elements(), 0.);

  // Deposit each participant onto the grid. Loop over nucleons in the nucleus, and check if nucleons are participants
  for (const auto& nucleon : nucleus) {
    if (!nucleon.is_participant())
      continue;

    ++npart_;

    // Get nucleon subgrid boundary {xmin, xmax, ymin, ymax}. NucleonCommon is defined in nucleon.h, where its method boundary is defined
    const auto boundary = nucleon_common.boundary(nucleon);

    // Determine min & max indices of nucleon subgrid.
    int ixmin = clip(static_cast<int>((boundary[0]+xymax_)/dxy_), 0, nsteps_-1);
    int ixmax = clip(static_cast<int>((boundary[1]+xymax_)/dxy_), 0, nsteps_-1);
    int iymin = clip(static_cast<int>((boundary[2]+xymax_)/dxy_), 0, nsteps_-1);
    int iymax = clip(static_cast<int>((boundary[3]+xymax_)/dxy_), 0, nsteps_-1);

    // Add profile to grid.
    for (auto iy = iymin; iy <= iymax; ++iy) {
      for (auto ix = ixmin; ix <= ixmax; ++ix) {
        TX[iy][ix] += nucleon_common.thickness(
          nucleon, (ix+.5)*dxy_ - xymax_, (iy+.5)*dxy_ - xymax_
        );
      }
    }
  }
}

/*void Event::compute_nuclear_deterministic_thickness(
    const Nucleus& nucleus, const NucleonCommon& nucleon_common, Grid& TX) {
   std::fill(TX.origin(), TX.origin() + TX.num_elements(), 0.); 

  // Deposit each participant onto the grid. Loop over nucleons in the nucleus, and check if nucleons are participants
  for (const auto& nucleon : nucleus) {

    // Get nucleon subgrid boundary {xmin, xmax, ymin, ymax}. NucleonCommon is defined in nucleon.h, where its method boundary is defined
    const auto boundary = nucleon_common.boundary(nucleon);

    // Determine min & max indices of nucleon subgrid.
    int ixmin = clip(static_cast<int>((boundary[0]+xymax_)/dxy_), 0, nsteps_-1); //ixmin will be boundary[0]+xymax_)/dxy_, or 0 if (boundary[0]+xymax_)/dxy_)< zero, or (nsteps-1) if (boundary[0]+xymax_)/dxy_) > (nsteps-1) 
    int ixmax = clip(static_cast<int>((boundary[1]+xymax_)/dxy_), 0, nsteps_-1);
    int iymin = clip(static_cast<int>((boundary[2]+xymax_)/dxy_), 0, nsteps_-1);
    int iymax = clip(static_cast<int>((boundary[3]+xymax_)/dxy_), 0, nsteps_-1);

    // Add profile to grid. 
    for (auto iy = iymin; iy <= iymax; ++iy) {
      for (auto ix = ixmin; ix <= ixmax; ++ix) {
        //TX[iy][ix] += nucleon_common.deterministic_thickness(
        //  nucleon, (ix+.5)*dxy_ - xymax_, (iy+.5)*dxy_ - xymax_
        //);
        TX[iy][ix] = 1;
      }
    }
  }
}*/




template <typename GenMean>
void Event::compute_reduced_thickness(GenMean gen_mean) {
  double sum = 0.;
  double ixcm = 0.;
  double iycm = 0.;

  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      auto t = norm_ * gen_mean(TA_[iy][ix], TB_[iy][ix]);
      TR_[iy][ix] = t;
      sum += t;
      // Center of mass grid indices.
      // No need to multiply by dxy since it would be canceled later.
      ixcm += t * static_cast<double>(ix); //ixcm is converted by static_cast in a double
      iycm += t * static_cast<double>(iy);
    }
  }

  multiplicity_ = dxy_ * dxy_ * sum; //integral of TR_, which gives us the integral of the entropy density == multiplicity 
  ixcm_ = ixcm / sum;
  iycm_ = iycm / sum;
}



//added by me
void Event::compute_ncoll() {
  ncoll_ ++;
  double sum = 0.;
  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      //auto t = norm_ * TA_det_[iy][ix] * TB_det_[iy][ix];

      TAB_[iy][ix] += 1;
      sum += 1;
    }
  }
  Tab_integr_ = dxy_ * dxy_ * sum; //integral of Tab 
  
}



void Event::compute_observables(){
  // Compute eccentricity.

  // Simple helper class for use in the following loop.
  struct EccentricityAccumulator {
    double re = 0.;  // real part
    double im = 0.;  // imaginary part
    double wt = 0.;  // weight
    double finish() const  // compute final eccentricity
    { return std::sqrt(re*re + im*im) / std::fmax(wt, TINY); }
  } e2, e3, e4, e5;

   double Si = 0.; // total entropy
  
  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      const auto& t = TR_[iy][ix];
      if (t < TINY)
        continue;

      // Compute (x, y) relative to the CM and cache powers of x, y, r.
      auto x = static_cast<double>(ix) - ixcm_;
      auto x2 = x*x;
      auto x3 = x2*x;
      auto x4 = x2*x2;

      auto y = static_cast<double>(iy) - iycm_;
      auto y2 = y*y;
      auto y3 = y2*y;
      auto y4 = y2*y2;

      auto r2 = x2 + y2;
      auto r = std::sqrt(r2);
      auto r4 = r2*r2;

      auto xy = x*y;
      auto x2y2 = x2*y2;

      // The eccentricity harmonics are weighted averages of r^n*exp(i*n*phi)
      // over the entropy profile (reduced thickness).  The naive way to compute
      // exp(i*n*phi) at a given (x, y) point is essentially:
      //
      //   phi = arctan2(y, x)
      //   real = cos(n*phi)
      //   imag = sin(n*phi)
      //
      // However this implementation uses three unnecessary trig functions; a
      // much faster method is to express the cos and sin directly in terms of x
      // and y.  For example, it is trivial to show (by drawing a triangle and
      // using rudimentary trig) that
      //
      //   cos(arctan2(y, x)) = x/r = x/sqrt(x^2 + y^2)
      //   sin(arctan2(y, x)) = y/r = x/sqrt(x^2 + y^2)
      //
      // This is easily generalized to cos and sin of (n*phi) by invoking the
      // multiple angle formula, e.g. sin(2x) = 2sin(x)cos(x), and hence
      //
      //   sin(2*arctan2(y, x)) = 2*sin(arctan2(y, x))*cos(arctan2(y, x))
      //                        = 2*x*y / r^2
      //
      // Which not only eliminates the trig functions, but also naturally
      // cancels the r^2 weight.  This cancellation occurs for all n.
      //
      // The Event unit test verifies that the two methods agree.
      e2.re += t * (y2 - x2);
      e2.im += t * 2.*xy;
      e2.wt += t * r2;

      e3.re += t * (y3 - 3.*y*x2);
      e3.im += t * (3.*x*y2 - x3);
      e3.wt += t * r2*r;

      e4.re += t * (x4 + y4 - 6.*x2y2);
      e4.im += t * 4.*xy*(y2 - x2);
      e4.wt += t * r4;

      e5.re += t * y*(5.*x4 - 10.*x2y2 + y4);
      e5.im += t * x*(x4 - 10.*x2y2 + 5.*y4);
      e5.wt += t * r4*r;
      
      
      //inital entropy from energy
      //double s_local = EOS_.StoE(t);
      double s_local = pow(t,4./3.);
      Si += s_local;
      double total_ent=0;
      total_ent += t;
    }
  }
  eccentricity_[2] = dxy_*dxy_*Si;
  eccentricity_[3] = e2.finish();
  eccentricity_[4] = e3.finish();
  eccentricity_[5] = e4.finish();
  //eccentricity_[4] = THETA_A;
  //eccentricity_[5] = PHI_A;
  //eccentricity_[6] = THETA_B;
  //eccentricity_[7] = PHI_B;
  
 // eccentricity_[4] = e4.finish();
 // eccentricity_[5] = e5.finish();



struct EccentricityAngleAccumulator {
    double re = 0.;  // real part
    double im = 0.;  // imaginary part
    double wt = 0.;  // weight
    double finish() const  // compute final eccentricity
    { return wt*(atan2(im,re)+M_PI); }
  } a2, a3, a4, a5;

   double Sia = 0.; // total entropy
  
  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      const auto& t = TR_[iy][ix];
      if (t < TINY)
        continue;

      // Compute (x, y) relative to the CM and cache powers of x, y, r.
      auto x = static_cast<double>(ix) - ixcm_;
      auto x2 = x*x;
      auto x3 = x2*x;
      auto x4 = x2*x2;

      auto y = static_cast<double>(iy) - iycm_;
      auto y2 = y*y;
      auto y3 = y2*y;
      auto y4 = y2*y2;

      auto r2 = x2 + y2;
      auto r = std::sqrt(r2);
      auto r4 = r2*r2;

      auto xy = x*y;
      auto x2y2 = x2*y2;

      // The eccentricity harmonics are weighted averages of r^n*exp(i*n*phi)
      // over the entropy profile (reduced thickness).  The naive way to compute
      // exp(i*n*phi) at a given (x, y) point is essentially:
      //
      //   phi = arctan2(y, x)
      //   real = cos(n*phi)
      //   imag = sin(n*phi)
      //
      // However this implementation uses three unnecessary trig functions; a
      // much faster method is to express the cos and sin directly in terms of x
      // and y.  For example, it is trivial to show (by drawing a triangle and
      // using rudimentary trig) that
      //
      //   cos(arctan2(y, x)) = x/r = x/sqrt(x^2 + y^2)
      //   sin(arctan2(y, x)) = y/r = x/sqrt(x^2 + y^2)
      //
      // This is easily generalized to cos and sin of (n*phi) by invoking the
      // multiple angle formula, e.g. sin(2x) = 2sin(x)cos(x), and hence
      //
      //   sin(2*arctan2(y, x)) = 2*sin(arctan2(y, x))*cos(arctan2(y, x))
      //                        = 2*x*y / r^2
      //
      // Which not only eliminates the trig functions, but also naturally
      // cancels the r^2 weight.  This cancellation occurs for all n.
      //
      // The Event unit test verifies that the two methods agree.
      a2.re += t * (y2 - x2);
      a2.im += t * 2.*xy;
      a2.wt = 1./2.;

      a3.re += t * (y3 - 3.*y*x2);
      a3.im += t * (3.*x*y2 - x3);
      a3.wt = 1./3.;

      a4.re += t * (x4 + y4 - 6.*x2y2);
      a4.im += t * 4.*xy*(y2 - x2);
      a4.wt = 1./4.;
      
      a5.re += t * y*(5.*x4 - 10.*x2y2 + y4);
      a5.im += t * x*(x4 - 10.*x2y2 + 5.*y4);
      a5.wt = 1./5.;
      
      
      //inital entropy from energy
      //double s_local = EOS_.StoE(t);
      double s_local = pow(t,4./3.);
      Sia += s_local;
      double total_ent=0;
      total_ent += t;
    }
  }
  eccentricity_[6] = a2.finish();
  eccentricity_[7] = a3.finish();
  eccentricity_[8] = a4.finish();
  //eccentricity_[4] = THETA_A;
  //eccentricity_[5] = PHI_A;
  //eccentricity_[6] = THETA_B;
  //eccentricity_[7] = PHI_B;
  
 // eccentricity_[4] = e4.finish();
 // eccentricity_[5] = e5.finish();



struct RadiusAccumulator {
    double re = 0.;  // real part
    double im = 0.;  // imaginary part
    double wt = 0.;  // weight
    double finish() const  // compute final eccentricity
    { return std::fmax(wt, TINY); }
  } R2, R3, R4, R5;
    double total_ent = 0.;
   double Sir = 0.; // total entropy
  
  for (int iy = 0; iy < nsteps_; ++iy) {
    for (int ix = 0; ix < nsteps_; ++ix) {
      const auto& t = TR_[iy][ix];
      if (t < TINY)
        continue;

      // Compute (x, y) relative to the CM and cache powers of x, y, r.
      auto x = static_cast<double>(ix) - ixcm_;
      auto x2 = x*x;
      auto x3 = x2*x;
      auto x4 = x2*x2;

      auto y = static_cast<double>(iy) - iycm_;
      auto y2 = y*y;
      auto y3 = y2*y;
      auto y4 = y2*y2;

      auto r2 = x2 + y2;
      auto r = std::sqrt(r2);
      auto r4 = r2*r2;

      auto xy = x*y;
      auto x2y2 = x2*y2;

      // The eccentricity harmonics are weighted averages of r^n*exp(i*n*phi)
      // over the entropy profile (reduced thickness).  The naive way to compute
      // exp(i*n*phi) at a given (x, y) point is essentially:
      //
      //   phi = arctan2(y, x)
      //   real = cos(n*phi)
      //   imag = sin(n*phi)
      //
      // However this implementation uses three unnecessary trig functions; a
      // much faster method is to express the cos and sin directly in terms of x
      // and y.  For example, it is trivial to show (by drawing a triangle and
      // using rudimentary trig) that
      //
      //   cos(arctan2(y, x)) = x/r = x/sqrt(x^2 + y^2)
      //   sin(arctan2(y, x)) = y/r = x/sqrt(x^2 + y^2)
      //
      // This is easily generalized to cos and sin of (n*phi) by invoking the
      // multiple angle formula, e.g. sin(2x) = 2sin(x)cos(x), and hence
      //
      //   sin(2*arctan2(y, x)) = 2*sin(arctan2(y, x))*cos(arctan2(y, x))
      //                        = 2*x*y / r^2
      //
      // Which not only eliminates the trig functions, but also naturally
      // cancels the r^2 weight.  This cancellation occurs for all n.
      //
      // The Event unit test verifies that the two methods agree.
      R2.re += t * (y2 - x2);
      R2.im += t * 2.*xy;
      R2.wt += t * r2;

      R3.re += t * (y3 - 3.*y*x2);
      R3.im += t * (3.*x*y2 - x3);
      R3.wt += t * r2*r;

      R4.re += t * (x4 + y4 - 6.*x2y2);
      R4.im += t * 4.*xy*(y2 - x2);
      R4.wt += t * r4;
      
      R5.re += t * y*(5.*x4 - 10.*x2y2 + y4);
      R5.im += t * x*(x4 - 10.*x2y2 + 5.*y4);
      R5.wt += t * r4*r;
      
      
      //inital entropy from energy
      //double s_local = EOS_.StoE(t);
      double s_local = pow(t,4./3.);
      Sir += s_local;
      total_ent += t;
    }
  }
  eccentricity_[9] = dxy_*dxy_*R2.finish()/total_ent;
  eccentricity_[10] = dxy_*dxy_*R3.finish()/total_ent;
  eccentricity_[11] = dxy_*dxy_*R4.finish()/total_ent;
  //eccentricity_[2] = e2.finish();
  //eccentricity_[3] = e3.finish();
  //eccentricity_[4] = THETA_A;
  //eccentricity_[5] = PHI_A;
  //eccentricity_[6] = THETA_B;
  //eccentricity_[7] = PHI_B;
  //eccentricity_[8] = dxy_*dxy_*Si;
 // eccentricity_[4] = e4.finish();
 // eccentricity_[5] = e5.finish();
}

}  // namespace trento
