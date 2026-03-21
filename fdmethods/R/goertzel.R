#source("RcppExports.R")

#' Evaluate Selected DFT Terms With The Goertzel Algorithm
#'
#' Computes one or more discrete Fourier transform terms for a real-valued
#' signal by applying the Goertzel recurrence. The `k` argument may contain
#' integer DFT bins or non-integer bins derived from physical frequencies.
#'
#' @param signal Numeric vector containing the input signal samples.
#' @param k Numeric scalar or vector giving the DFT bin locations to evaluate.
#'
#' @return A complex scalar when `k` has length one, otherwise a complex vector
#'   with one value per requested bin.
#'
#' @examples
#' sample_rate <- 8000
#' sample_count <- 400
#' dtmf_low <- c(697, 770, 852, 941)
#' dtmf_high <- c(1209, 1336, 1477, 1633)
#' frequencies <- c(dtmf_low, dtmf_high)
#'
#' set.seed(6)
#' time <- (0:(sample_count - 1)) / sample_rate
#' signal <- 0.9 * sin(2 * pi * 770 * time) +
#'   0.9 * sin(2 * pi * 1477 * time) +
#'   rnorm(sample_count, mean = 0, sd = 0.30)
#'
#' bins <- frequencies * sample_count / sample_rate
#' response <- goertzel(signal, bins)
#' power <- Mod(response) ^ 2
#'
#' low_index <- which.max(power[seq_along(dtmf_low)])
#' high_index <- which.max(power[length(dtmf_low) + seq_along(dtmf_high)])
#'
#' stopifnot(low_index == 2L, high_index == 3L)
#'
#' key <- matrix(
#'   c("1", "2", "3", "A",
#'     "4", "5", "6", "B",
#'     "7", "8", "9", "C",
#'     "*", "0", "#", "D"),
#'   nrow = length(dtmf_low),
#'   byrow = TRUE
#' )
#' key[low_index, high_index]
#' @export
goertzel <- function(signal, k) {
  N <- length(signal)
  alpha = 2 * cos(2 * pi * k / N)
  w <- exp(2i * pi * k / N)
  X <- goertzelCore(signal, alpha)
  if (length(k) == 1) {
    return(w * X[2] - X[1])
  }
  else {
    l <- length(X) / 2
    return(w * X[(1:l) * 2] - X[(1:l) * 2 - 1])
  }
}
