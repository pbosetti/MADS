#include <Rcpp.h>
#include "goertzel/goertzel.hpp"

using namespace Rcpp;

// [[Rcpp::export]]
SEXP goertzelCore(SEXP a, SEXP alpha) {
  Rcpp::NumericVector xa(a);
  Rcpp::NumericVector xalpha(alpha);

  std::vector<double> alpha_values(xalpha.begin(), xalpha.end());
  Goertzel::Analyzer analyzer(std::move(alpha_values));
  analyzer.process(xa.begin(), xa.end());

  const auto states = analyzer.state_vector();
  return Rcpp::wrap(states);
}
