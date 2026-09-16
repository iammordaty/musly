/**
 * Copyright 2013-2014, Dominik Schnitzer <dominik@schnitzer.at>
 *                2026, Musly maintainers
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cmath>
#include <limits>
#include <algorithm>
#include <Eigen/Core>
#include <Eigen/Cholesky>
#include "minilog.h"
#include "gaussianstatistics.h"


namespace musly {

gaussian_statistics::gaussian_statistics(
        int gaussian_dim,
        float shrinkage_lambda_) :
                d(gaussian_dim),
                covar_elems((d*(d+1)/2)),
                shrinkage_lambda(shrinkage_lambda_)
{
}

int
gaussian_statistics::get_covarelems()
{
    return covar_elems;
}

int
gaussian_statistics::get_dim()
{
    return d;
}


bool
gaussian_statistics::estimate_gaussian(
        const Eigen::MatrixXf& m,
        gaussian& g)
{
    MINILOG(logTRACE) << "Estimating Gaussian from matrix: " << m.rows()
            << "x" << m.cols();

    if (m.cols() <= d) {
        MINILOG(logTRACE) << "could not estimate Gaussian. "
                << "Too few input samples. m.cols=" << m.cols();
        return false;
    }

    if (m.rows() != d) {
        MINILOG(logTRACE) << "could not estimate Gaussian. "
                << "Wrong dimension (d=" << d << " vs. m.rows="
                << m.rows() << ")";
        return false;
    }

    // Estimate in double for numeric stability of large log-determinants.
    Eigen::MatrixXd md = m.cast<double>();
    Eigen::VectorXd mu = md.rowwise().mean();
    if (g.mu) {
        for (int i = 0; i < d; i++) {
            if (!std::isfinite(mu(i))) {
                return false;
            }
            g.mu[i] = static_cast<float>(mu(i));
        }
    }

    Eigen::MatrixXd covar = (md.colwise() - mu) * (md.colwise() - mu).transpose()
            / (static_cast<double>(md.cols()) - 1.0);

    // Ledoit-Wolf style relative shrinkage toward scaled identity.
    // Scale-invariant and improves conditioning with correlated MFCC frames.
    const double lambda = shrinkage_lambda;
    double mean_var = covar.trace() / static_cast<double>(d);
    if (!(mean_var > 0.0) || !std::isfinite(mean_var)) {
        mean_var = 1e-6;
    }
    covar = (1.0 - lambda) * covar
            + lambda * mean_var * Eigen::MatrixXd::Identity(d, d);

    if (g.covar) {
        int idx_ij = 0;
        for (int i = 0; i < d; i++) {
            for (int j = i; j < d; j++) {
                float v = static_cast<float>(covar(i, j));
                if (!std::isfinite(v)) {
                    return false;
                }
                g.covar[idx_ij] = v;
                idx_ij++;
            }
        }
    }

    if (g.covar_inverse || g.covar_logdet) {
        Eigen::LLT<Eigen::MatrixXd> llt(covar);
        if (llt.info() != Eigen::Success) {
            MINILOG(logDEBUG) << "Could not compute Cholesky of covariance";
            return false;
        }

        if (g.covar_inverse) {
            Eigen::MatrixXd covar_inverse = llt.solve(
                    Eigen::MatrixXd::Identity(d, d));
            int idx_ij = 0;
            for (int i = 0; i < d; i++) {
                for (int j = i; j < d; j++) {
                    float v = static_cast<float>(covar_inverse(i, j));
                    if (!std::isfinite(v)) {
                        return false;
                    }
                    g.covar_inverse[idx_ij] = v;
                    idx_ij++;
                }
            }
        }

        if (g.covar_logdet) {
            // log|Σ| = 2 * sum(log(diag(L))) for Σ = L L^T
            Eigen::MatrixXd L = llt.matrixL();
            double logdet = 0.0;
            for (int i = 0; i < d; i++) {
                double diag = L(i, i);
                if (!(diag > 0.0) || !std::isfinite(diag)) {
                    return false;
                }
                logdet += std::log(diag);
            }
            logdet *= 2.0;
            if (!std::isfinite(logdet)) {
                return false;
            }
            *(g.covar_logdet) = static_cast<float>(logdet);
        }
    }

    return true;
}


float
gaussian_statistics::jensenshannon(
        const gaussian& g0,
        const gaussian& g1,
        gaussian& tmp)
{
    // return 0 if the models to compare are the same
    if ((g0.covar == g1.covar) && (g0.mu == g1.mu)) {
        return 0;
    }
    float jsd = -0.25f * (*(g0.covar_logdet) + *(g1.covar_logdet));

    // merge the mean and covariance matrices to get the merged Gaussian
    for (int i = 0; i < d; i++) {
        tmp.mu[i] = 0.5*(g0.mu[i] - g1.mu[i]);
    }
    int idx_covar = 0;
    for (int i = 0; i < d; i++) {
        for (int j = i; j < d; j++) {
            tmp.covar[idx_covar] = 0.5f*
                    (g0.covar[idx_covar] + g1.covar[idx_covar]) +
                    tmp.mu[i]*tmp.mu[j];
            idx_covar++;
        }
    }

    // Do an inplace cholesky decompositon and compute logdet of the merged
    // Gaussian.
    int idx_ii = 0;
    for (int i = 0; i < d; i++) {
        int idx_k = i;
        for (int k = 0; k < i; k++) {
            tmp.covar[idx_ii] -=
                    tmp.covar[idx_k]*tmp.covar[idx_k];
            idx_k += d - k - 1;
        }

        if (tmp.covar[idx_ii] <= 0) {
            // Degenerate merge: treat as maximally distant (not "most similar").
            return std::numeric_limits<float>::max();
        }
        tmp.covar[idx_ii] = std::sqrt(tmp.covar[idx_ii]);
        jsd += std::log(tmp.covar[idx_ii]);

        int idx_ij = idx_ii;
        for (int j = i+1; j < d; j++) {
            idx_ij++;

            int idx_k = 0;
            for (int k = 0; k < i; k++) {
                tmp.covar[idx_ij] -=
                        tmp.covar[idx_k+i] * tmp.covar[idx_k+j];
                idx_k += d - k - 1;
            }
            tmp.covar[idx_ij] /= tmp.covar[idx_ii];
        }

        idx_ii += d - i;
    }

    if (std::isnan(jsd) || std::isinf(jsd)) {
        return std::numeric_limits<float>::max();
    }

    return std::sqrt(std::max(0.0f, jsd));
}

float
gaussian_statistics::symmetric_kullbackleibler(
        const gaussian& g0,
        const gaussian& g1,
        gaussian& tmp)
{
    // distance value
    float skld = 0;

    // return 0 if the models to compare are the same
    if ((g0.covar == g1.covar) && (g0.mu == g1.mu)) {
        return skld;
    }


    // add the two inverted covariances
    for (int i = 0; i < covar_elems; i++) {
        tmp.covar_inverse[i] = g0.covar_inverse[i] + g1.covar_inverse[i];
    }

    for (int i = 0; i < d; i++) {
        int idx = i*d - (i*i+i)/2;

        skld += g0.covar[idx+i] * g1.covar_inverse[idx+i] +
                g1.covar[idx+i] * g0.covar_inverse[idx+i];

        for (int k = i+1; k < d; k++) {
            skld += 2*g0.covar[idx+k] * g1.covar_inverse[idx+k] +
                2*g1.covar[idx+k] * g0.covar_inverse[idx+k];
        }
    }

    // compute the difference of the two means
    for (int i = 0; i < d; i++) {
        tmp.mu[i] = g0.mu[i] - g1.mu[i];
    }

    for (int i = 0; i < d; i++) {
        int idx = i - d;
        float tmp1 = 0;

        for (int k = 0; k <= i; k++) {
            idx += d - k;
            tmp1 += tmp.covar_inverse[idx] * tmp.mu[k];
        }

        for (int k = i + 1; k < d; k++) {
            idx++;
            tmp1 += tmp.covar_inverse[idx] * tmp.mu[k];
        }
        skld += tmp1 * tmp.mu[i];
    }

    if (std::isnan(skld) || std::isinf(skld)) {
        return std::numeric_limits<float>::max();
    }

    return std::max(skld/4.0f - d/2.0f, 0.0f);
}

} /* namespace musly */
