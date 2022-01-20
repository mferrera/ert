/*
   Copyright (C) 2019  Equinor ASA, Norway.

   The file 'ies_enkf.cpp' is part of ERT - Ensemble based Reservoir Tool.

   ERT is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   ERT is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.

   See the GNU General Public License at <http://www.gnu.org/licenses/gpl.html>
   for more details.
*/
#include <algorithm>
#include <variant>
#include <vector>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <ert/python.hpp>
#include <ert/util/util.hpp>
#include <ert/util/rng.hpp>
#include <ert/util/bool_vector.hpp>

#include <ert/res_util/matrix.hpp>
#include <ert/res_util/matrix_blas.hpp>

#include <ert/analysis/analysis_module.hpp>
#include <ert/analysis/analysis_table.hpp>
#include <ert/analysis/enkf_linalg.hpp>

#include <ert/analysis/ies/ies.hpp>
#include <ert/analysis/ies/ies_config.hpp>
#include <ert/analysis/ies/ies_data.hpp>

namespace ies {
void linalg_compute_AA_projection(const matrix_type *A, matrix_type *Y);

void linalg_solve_S(const matrix_type *W0, const matrix_type *Y,
                    matrix_type *S);

void linalg_subspace_inversion(matrix_type *W0, const int ies_inversion,
                               const matrix_type *E, const matrix_type *R,
                               const matrix_type *S, const matrix_type *H,
                               const std::variant<double, int> &truncation,
                               double ies_steplength);

void linalg_exact_inversion(matrix_type *W0, const int ies_inversion,
                            const matrix_type *S, const matrix_type *H,
                            double ies_steplength);
} // namespace ies

namespace {
auto logger = ert::get_logger("ies");
}

#define ENKF_SUBSPACE_DIMENSION_KEY "ENKF_SUBSPACE_DIMENSION"
#define ENKF_TRUNCATION_KEY "ENKF_TRUNCATION"
#define IES_MAX_STEPLENGTH_KEY "IES_MAX_STEPLENGTH"
#define IES_MIN_STEPLENGTH_KEY "IES_MIN_STEPLENGTH"
#define IES_DEC_STEPLENGTH_KEY "IES_DEC_STEPLENGTH"
#define ITER_KEY "ITER"
#define IES_DEBUG_KEY "IES_DEBUG"

#define IES_INVERSION_KEY "IES_INVERSION"
#define IES_LOGFILE_KEY "IES_LOGFILE"
#define IES_AAPROJECTION_KEY "IES_AAPROJECTION"

//#define DEFAULT_ANALYSIS_SCALE_DATA true

void ies::init_update(void *arg, const bool_vector_type *ens_mask,
                      const bool_vector_type *obs_mask, const matrix_type *S,
                      const matrix_type *R, const matrix_type *dObs,
                      const matrix_type *E, const matrix_type *D,
                      rng_type *rng) {
    ies::data_type *module_data = ies::data_safe_cast(arg);

    /* Store current ens_mask in module_data->ens_mask for each iteration */
    ies::data_update_ens_mask(module_data, ens_mask);
    ies::data_allocateW(module_data);

    /* Store obs_mask for initial iteration in module_data->obs_mask0,
     *  for each subsequent iteration we store the current mask in module_data->obs_mask */
    ies::store_initial_obs_mask(module_data, obs_mask);
    ies::update_obs_mask(module_data, obs_mask);
}

void ies_initX__(const matrix_type *A, const matrix_type *Y0,
                 const matrix_type *R, const matrix_type *E,
                 const matrix_type *D, matrix_type *X,
                 const ies::config::inversion_type ies_inversion,
                 const std::variant<double, int> &truncation,
                 bool use_aa_projection, ies::data_type *data,
                 double ies_steplength, int iteration_nr, double *costf)

{
    const int ens_size = matrix_get_columns(Y0);
    const int nrobs_inp = matrix_get_rows(Y0);

    matrix_type *Y = matrix_alloc_copy(Y0);
    matrix_type *H =
        matrix_alloc(nrobs_inp, ens_size); // Innovation vector "H= S*W+D-Y"
    matrix_type *S = matrix_alloc(
        nrobs_inp,
        ens_size); // Predicted ensemble anomalies scaled with inv(Omeaga)
    const double nsc = 1.0 / sqrt(ens_size - 1.0);

    /*  Subtract mean of predictions to generate predicted ensemble anomaly matrix (Line 5) */
    matrix_subtract_row_mean(Y); // Y=Y*(I-(1/ens_size)*11)
    matrix_scale(Y, nsc);        // Y=Y / sqrt(ens_size-1)

    /* COMPUTING THE PROJECTION Y= Y * (Ai^+ * Ai) (only used when state_size < ens_size-1) */
    if (A) {
        const int state_size = matrix_get_rows(A);
        if (use_aa_projection && (state_size <= (ens_size - 1))) {
            ies::linalg_compute_AA_projection(A, Y);
        }
    }

    /* COPY ACTIVE REALIZATIONS FROM data->W to W0 */
    matrix_type *W0 = ies::alloc_activeW(data);

    /*
     * When solving the system S = Y inv(Omega) we write
     *   Omega^T S^T = Y^T (line 6)
     */
    ies::linalg_solve_S(W0, Y, S);

    /* INNOVATION H = S*W + D - Y   from Eq. (47) (Line 8)*/
    matrix_assign(H, D);                            // H=D=dobs + E - Y
    matrix_dgemm(H, S, W0, false, false, 1.0, 1.0); // H=S*W + H

    /* Store previous W for convergence test */
    matrix_type *W = matrix_alloc_copy(W0);

    /*
     * COMPUTE NEW UPDATED W                                                                        (Line 9)
     *    W = W + ies_steplength * ( W - S'*(S*S'+R)^{-1} H )          (a)
     * which in the case when R=I can be rewritten as
     *    W = W + ies_steplength * ( W - (S'*S + I)^{-1} * S' * H )    (b)
     *
     * With R=I the subspace inversion (ies_inversion=1) solving Eq. (a) with singular value
     * trucation=1.000 gives exactly the same solution as the exact inversion (ies_inversion=0).
     *
     * Using ies_inversion=IES_INVERSION_SUBSPACE_EXACT_R(2), and a step length of 1.0,
     * one update gives identical result to STD as long as the same SVD
     * truncation is used.
     *
     * With very large data sets it is likely that the inversion becomes poorly
     * conditioned and a trucation=1.000 is not a good choice. In this case the
     * ies_inversion > 0 and truncation set to 0.99 or so, should stabelize
     * the algorithm.
     *
     * Using ies_inversion=IES_INVERSION_SUBSPACE_EE_R(3) and
     * ies_inversion=IES_INVERSION_SUBSPACE_RE(2) gives identical results but
     * ies_inversion=IES_INVERSION_SUBSPACE_RE is much faster (N^2m) than
     * ies_inversion=IES_INVERSION_SUBSPACE_EE_R (Nm^2).
     *
     * See the enum: ies_inverson in ies_config.hpp:
     *
     * ies_inversion=IES_INVERSION_EXACT(0)            -> exact inversion from (b) with exact R=I
     * ies_inversion=IES_INVERSION_SUBSPACE_EXACT_R(1) -> subspace inversion from (a) with exact R
     * ies_inversion=IES_INVERSION_SUBSPACE_EE_R(2)    -> subspace inversion from (a) with R=EE
     * ies_inversion=IES_INVERSION_SUBSPACE_RE(3)      -> subspace inversion from (a) with R represented by E
     */

    if (ies_inversion != ies::config::IES_INVERSION_EXACT) {
        ies::linalg_subspace_inversion(W0, ies_inversion, E, R, S, H,
                                       truncation, ies_steplength);
    } else if (ies_inversion == ies::config::IES_INVERSION_EXACT) {
        ies::linalg_exact_inversion(W0, ies_inversion, S, H, ies_steplength);
    }

    /* Store active realizations from W0 to data->W */
    ies::linalg_store_active_W(data, W0);

    /*
     * CONSTRUCT TRANFORM MATRIX X FOR CURRENT ITERATION (Line 10)
     *   X= I + W/sqrt(N-1)
     */
    matrix_assign(X, W0);
    matrix_scale(X, nsc);
    for (int i = 0; i < ens_size; i++) {
        matrix_iadd(X, i, i, 1.0);
    }

    /* COMPUTE ||W0 - W|| AND EVALUATE COST FUNCTION FOR PREVIOUS ITERATE (Line 12)*/
    matrix_type *DW = matrix_alloc(ens_size, ens_size);
    matrix_sub(DW, W0, W);

    if (costf) {
        std::vector<double> costJ(ens_size);
        double local_costf = 0.0;
        for (int i = 0; i < ens_size; i++) {
            costJ[i] = matrix_column_column_dot_product(W, i, W, i) +
                       matrix_column_column_dot_product(D, i, D, i);
            local_costf += costJ[i];
        }
        local_costf = local_costf / ens_size;
        *costf = local_costf;
    }

    matrix_free(W0);
    matrix_free(W);
    matrix_free(DW);
    matrix_free(H);
    matrix_free(S);
    matrix_free(Y);
}

void ies::updateA(
    void *module_data,
    matrix_type *A,          // Updated ensemble A retured to ERT.
    const matrix_type *Yin,  // Ensemble of predicted measurements
    const matrix_type *Rin,  // Measurement error covariance matrix (not used)
    const matrix_type *dObs, // Actual observations (not used)
    const matrix_type *Ein,  // Ensemble of observation perturbations
    const matrix_type *Din,  // (d+E-Y) Ensemble of perturbed observations - Y
    rng_type *rng) {

    ies::data_type *data = ies::data_safe_cast(module_data);
    const ies::config::config_type *ies_config = ies::data_get_config(data);

    int ens_size = matrix_get_columns(
        Yin); // Number of active realizations in current iteration
    int state_size = matrix_get_rows(A);

    int iteration_nr = ies::data_inc_iteration_nr(data);

    const double ies_steplength =
        ies::config::calculate_steplength(ies_config, iteration_nr);

    ies::data_update_state_size(data, state_size);

    /*
      Counting number of active observations for current iteration. If the
      observations have been used in previous iterations they are contained in
      data->E0. If they are introduced in the current iteration they will be
      augmented to data->E.
    */
    ies::data_store_initialE(data, Ein);
    ies::data_augment_initialE(data, Ein);
    ies::data_store_initialA(data, A);

    /*
     * Re-structure input matrices according to new active obs_mask and ens_size.
     * Allocates the local matrices to be used.
     * Copies the initial measurement perturbations for the active observations into the current E matrix.
     * Copies the inputs in D, Y and R into their local representations
     */
    matrix_type *Y = matrix_alloc_copy(Yin);
    matrix_type *R = matrix_alloc_copy(Rin);
    matrix_type *E = ies::alloc_activeE(data);
    matrix_type *D = matrix_alloc_copy(Din);
    matrix_type *X = matrix_alloc(ens_size, ens_size);

    /* Subtract new measurement perturbations              D=D-E    */
    matrix_inplace_sub(D, Ein);
    /* Add old measurement perturbations */
    matrix_inplace_add(D, E);

    double costf;
    ies_initX__(ies::config::get_aaprojection(ies_config) ? A : nullptr, Y, R,
                E, D, X, ies::config::get_inversion(ies_config),
                ies::config::get_truncation(ies_config),
                ies::config::get_aaprojection(ies_config), data, ies_steplength,
                iteration_nr, &costf);
    logger->info("IES  iter:{} cost function: {}", iteration_nr, costf);

    /* COMPUTE NEW ENSEMBLE SOLUTION FOR CURRENT ITERATION  Ei=A0*X (Line 11)*/
    int m_ens_size = std::min(ens_size - 1, 16);
    int m_state_size = std::min(state_size - 1, 3);
    {
        matrix_type *A0 = ies::alloc_activeA(data);
        matrix_matmul(A, A0, X);
        matrix_free(A0);
    }

    matrix_free(Y);
    matrix_free(D);
    matrix_free(E);
    matrix_free(R);
    matrix_free(X);
}

static matrix_type *alloc_active(const matrix_type *full_matrix,
                                 const bool_vector_type *row_mask,
                                 const bool_vector_type *column_mask) {
    int rows = bool_vector_size(row_mask);
    int columns = bool_vector_size(column_mask);

    matrix_type *active =
        matrix_alloc(bool_vector_count_equal(row_mask, true),
                     bool_vector_count_equal(column_mask, true));
    int row = 0;
    for (int iobs = 0; iobs < rows; iobs++) {
        if (bool_vector_iget(row_mask, iobs)) {
            int column = 0;
            for (int iens = 0; iens < columns; iens++) {
                if (bool_vector_iget(column_mask, iens)) {
                    matrix_iset(active, row, column,
                                matrix_iget(full_matrix, iobs, iens));
                    column++;
                }
            }
            row++;
        }
    }

    return active;
}

/*
  During the iteration process both the number of realizations and the number of
  observations can change, the number of realizations can only be reduced but
  the number of (active) observations can both be reduced and increased. The
  iteration algorithm is based maintaining a state for the entire update
  process, in order to do this correctly we must create matrix representations
  with the correct active elements both in observation and realisation space.
*/

matrix_type *ies::alloc_activeE(const ies::data_type *data) {
    return alloc_active(ies::data_getE(data), ies::data_get_obs_mask(data),
                        ies::data_get_ens_mask(data));
}

matrix_type *ies::alloc_activeW(const ies::data_type *data) {
    return alloc_active(ies::data_getW(data), ies::data_get_ens_mask(data),
                        ies::data_get_ens_mask(data));
}

matrix_type *ies::alloc_activeA(const ies::data_type *data) {
    const matrix_type *A0 = ies::data_getA0(data);
    bool_vector_type *state_mask = bool_vector_alloc(matrix_get_rows(A0), true);
    matrix_type *activeA = alloc_active(ies::data_getA0(data), state_mask,
                                        ies::data_get_ens_mask(data));
    bool_vector_free(state_mask);
    return activeA;
}

/*  COMPUTING THE PROJECTION Y= Y * (Ai^+ * Ai) (only used when state_size < ens_size-1)    */
void ies::linalg_compute_AA_projection(const matrix_type *A, matrix_type *Y) {
    int ens_size = matrix_get_columns(A);
    int state_size = matrix_get_rows(A);
    int nrobs = matrix_get_rows(Y);

    int m_nrobs = std::min(nrobs - 1, 7);
    int m_ens_size = std::min(ens_size - 1, 16);
    int m_state_size = std::min(state_size - 1, 3);

    std::vector<double> eig(ens_size);
    matrix_type *Ai = matrix_alloc_copy(A);
    matrix_type *AAi = matrix_alloc(ens_size, ens_size);
    matrix_subtract_row_mean(Ai);
    matrix_type *VT = matrix_alloc(state_size, ens_size);
    matrix_dgesvd(DGESVD_NONE, DGESVD_MIN_RETURN, Ai, eig.data(), NULL, VT);
    matrix_dgemm(AAi, VT, VT, true, false, 1.0, 0.0);

    matrix_inplace_matmul(Y, AAi);
    matrix_free(Ai);
    matrix_free(AAi);
    matrix_free(VT);
}

/*
* COMPUTE  Omega= I + W (I-11'/sqrt(ens_size))    from Eq. (36).                                   (Line 6)
*  When solving the system S = Y inv(Omega) we write
*     Omega^T S^T = Y^T
*/
void ies::linalg_solve_S(const matrix_type *W0, const matrix_type *Y,
                         matrix_type *S) {
    int ens_size = matrix_get_columns(W0);
    int nrobs = matrix_get_rows(S);
    double nsc = 1.0 / sqrt(ens_size - 1.0);

    matrix_type *YT =
        matrix_alloc(ens_size, nrobs); // Y^T used in linear solver
    matrix_type *ST =
        matrix_alloc(ens_size, nrobs); // current S^T used in linear solver
    matrix_type *Omega =
        matrix_alloc(ens_size, ens_size); // current S^T used in linear solver

    /*  Here we compute the W (I-11'/N) / sqrt(N-1)  and transpose it).*/
    matrix_assign(
        Omega,
        W0); // Omega=data->W (from previous iteration used to solve for S)
    matrix_subtract_row_mean(Omega);   // Omega=Omega*(I-(1/N)*11')
    matrix_scale(Omega, nsc);          // Omega/sqrt(N-1)
    matrix_inplace_transpose(Omega);   // Omega=transpose(Omega)
    for (int i = 0; i < ens_size; i++) // Omega=Omega+I
        matrix_iadd(Omega, i, i, 1.0);

    matrix_transpose(Y, YT); // RHS stored in YT

    /* Solve system                                                                                (Line 7)   */
    matrix_dgesvx(Omega, YT, nullptr);

    matrix_transpose(YT, S); // Copy solution to S

    matrix_free(Omega);
    matrix_free(ST);
    matrix_free(YT);
}

/*
*  The standard inversion works on the equation
*          S'*(S*S'+R)^{-1} H           (a)
*/
void ies::linalg_subspace_inversion(matrix_type *W0, const int ies_inversion,
                                    const matrix_type *E, const matrix_type *R,
                                    const matrix_type *S, const matrix_type *H,
                                    const std::variant<double, int> &truncation,
                                    double ies_steplength) {

    int ens_size = matrix_get_columns(S);
    int nrobs = matrix_get_rows(S);
    double nsc = 1.0 / sqrt(ens_size - 1.0);
    matrix_type *X1 = matrix_alloc(
        nrobs, std::min(ens_size, nrobs)); // Used in subspace inversion
    std::vector<double> eig(ens_size);

    if (ies_inversion == config::IES_INVERSION_SUBSPACE_RE) {
        matrix_type *scaledE = matrix_alloc_copy(E);
        matrix_scale(scaledE, nsc);

        enkf_linalg_lowrankE(S, scaledE, X1, eig.data(), truncation);

        matrix_free(scaledE);
    } else if (ies_inversion == config::IES_INVERSION_SUBSPACE_EE_R) {
        matrix_type *Et = matrix_alloc_transpose(E);
        matrix_type *Cee = matrix_alloc_matmul(E, Et);
        matrix_scale(Cee, 1.0 / ((ens_size - 1) * (ens_size - 1)));

        enkf_linalg_lowrankCinv(S, Cee, X1, eig.data(), truncation);

        matrix_free(Et);
        matrix_free(Cee);
    } else if (ies_inversion == config::IES_INVERSION_SUBSPACE_EXACT_R) {
        matrix_type *scaledR = matrix_alloc_copy(R);
        matrix_scale(scaledR, nsc * nsc);
        enkf_linalg_lowrankCinv(S, scaledR, X1, eig.data(), truncation);
        matrix_free(scaledR);
    }

    {
        /*
          X3 = X1 * diag(eig) * X1' * H (Similar to Eq. 14.31, Evensen (2007))
        */
        matrix_type *X3 =
            matrix_alloc(nrobs, ens_size); // Used in subspace inversion
        enkf_linalg_genX3(X3, X1, H, eig.data());

        /*    Update data->W = (1-ies_steplength) * data->W +  ies_steplength * S' * X3                          (Line 9)    */
        matrix_dgemm(W0, S, X3, true, false, ies_steplength,
                     1.0 - ies_steplength);

        matrix_free(X3);
    }
    matrix_free(X1);
}

/*
*  The standard inversion works on the equation
*          S'*(S*S'+R)^{-1} H           (a)
*  which in the case when R=I can be rewritten as
*          (S'*S + I)^{-1} * S' * H     (b)
*/
void ies::linalg_exact_inversion(matrix_type *W0, const int ies_inversion,
                                 const matrix_type *S, const matrix_type *H,
                                 double ies_steplength) {
    int ens_size = matrix_get_columns(S);

    matrix_type *Z = matrix_alloc(ens_size, ens_size); // Eigen vectors of S'S+I
    matrix_type *ZtStH = matrix_alloc(ens_size, ens_size);
    matrix_type *StH = matrix_alloc(ens_size, ens_size);
    matrix_type *StS = matrix_alloc(ens_size, ens_size);
    std::vector<double> eig(ens_size);

    matrix_diag_set_scalar(StS, 1.0);
    matrix_dgemm(StS, S, S, true, false, 1.0, 1.0);
    matrix_dgesvd(DGESVD_ALL, DGESVD_NONE, StS, eig.data(), Z, NULL);

    matrix_dgemm(StH, S, H, true, false, 1.0, 0.0);
    matrix_dgemm(ZtStH, Z, StH, true, false, 1.0, 0.0);

    for (int i = 0; i < ens_size; i++) {
        eig[i] = 1.0 / eig[i];
        matrix_scale_row(ZtStH, i, eig[i]);
    }

    /*    Update data->W = (1-ies_steplength) * data->W +  ies_steplength * Z * (Lamda^{-1}) Z' S' H         (Line 9)    */
    matrix_dgemm(W0, Z, ZtStH, false, false, ies_steplength,
                 1.0 - ies_steplength);

    matrix_free(Z);
    matrix_free(ZtStH);
    matrix_free(StH);
    matrix_free(StS);
}

/*
* the updated W is stored for each iteration in data->W. If we have lost realizations we copy only the active rows and cols from
* W0 to data->W which is then used in the algorithm.  (note the definition of the pointer dataW to data->W)
*/
void ies::linalg_store_active_W(ies::data_type *data, const matrix_type *W0) {
    int ens_size_msk = ies::data_get_ens_mask_size(data);
    int i = 0;
    int j;
    matrix_type *dataW = ies::data_getW(data);
    const bool_vector_type *ens_mask = ies::data_get_ens_mask(data);
    matrix_set(dataW, 0.0);
    for (int iens = 0; iens < ens_size_msk; iens++) {
        if (bool_vector_iget(ens_mask, iens)) {
            j = 0;
            for (int jens = 0; jens < ens_size_msk; jens++) {
                if (bool_vector_iget(ens_mask, jens)) {
                    matrix_iset_safe(dataW, iens, jens, matrix_iget(W0, i, j));
                    j += 1;
                }
            }
            i += 1;
        }
    }
}

/*
  In the inner loop of the ies implementation is a function ies_initX__() which
  calculates the X matrix based on the fundamental matrices Y/S, R, E and D and
  additional arguments from the iterative state, including the steplength.

  Here the ies_initX__() function can be called without any iteration state, the
  minimum required iteration state - including steplength = 1 - is initialized
  as temporary local variables.
*/

void ies::initX(const config::config_type *ies_config, const matrix_type *Y0,
                const matrix_type *R, const matrix_type *E,
                const matrix_type *D, matrix_type *X) {
    ies::data_type *data = static_cast<ies::data_type *>(ies::data_alloc());
    const int ens_size = matrix_get_columns(Y0);
    const int obs_size = matrix_get_rows(Y0);
    bool_vector_type *ens_mask = bool_vector_alloc(ens_size, true);
    bool_vector_type *obs_mask = bool_vector_alloc(obs_size, true);
    ies::update_obs_mask(data, obs_mask);
    ies::data_update_ens_mask(data, ens_mask);
    ies::data_allocateW(data);

    bool use_aa_projection = false;
    double steplength = 1;
    int iteration_nr = 1;

    ies_initX__(nullptr, Y0, R, E, D, X, config::get_inversion(ies_config),
                config::get_truncation(ies_config), use_aa_projection, data,
                steplength, iteration_nr, nullptr);

    bool_vector_free(obs_mask);
    bool_vector_free(ens_mask);
    ies::data_free(data);
}

namespace {

bool set_int(void *arg, const char *var_name, int value) {
    ies::data_type *module_data = ies::data_safe_cast(arg);
    ies::config::config_type *config = ies::data_get_config(module_data);
    {
        bool name_recognized = true;

        if (strcmp(var_name, ENKF_SUBSPACE_DIMENSION_KEY) == 0)
            ies::config::set_subspace_dimension(config, value);
        else if (strcmp(var_name, ITER_KEY) == 0)
            ies::data_set_iteration_nr(module_data, value);
        else if (
            strcmp(var_name, IES_INVERSION_KEY) ==
            0) // This should probably translate string value - now it goes directly on the value of the inversion_type enum.
            ies::config::set_inversion(
                config, static_cast<ies::config::inversion_type>(value));
        else
            name_recognized = false;

        return name_recognized;
    }
}

int get_int(const void *arg, const char *var_name) {
    const ies::data_type *module_data = ies::data_safe_cast_const(arg);
    const ies::config::config_type *ies_config =
        ies::data_get_config(module_data);
    {
        if (strcmp(var_name, ITER_KEY) == 0)
            return ies::data_get_iteration_nr(module_data);
        else if (strcmp(var_name, ENKF_SUBSPACE_DIMENSION_KEY) == 0) {
            const auto &truncation = ies::config::get_truncation(ies_config);
            if (std::holds_alternative<int>(truncation))
                return std::get<int>(truncation);
            else
                return -1;
        } else if (strcmp(var_name, IES_INVERSION_KEY) == 0)
            return ies::config::get_inversion(ies_config);
        else
            return -1;
    }
}

bool set_string(void *arg, const char *var_name, const char *value) {
    ies::data_type *module_data = ies::data_safe_cast(arg);
    ies::config::config_type *ies_config = ies::data_get_config(module_data);
    {
        bool name_recognized = true;

        if (strcmp(var_name, IES_LOGFILE_KEY) == 0)
            ies::config::set_logfile(ies_config, value);
        else
            name_recognized = false;

        return name_recognized;
    }
}

bool set_bool(void *arg, const char *var_name, bool value) {
    ies::data_type *module_data = ies::data_safe_cast(arg);
    ies::config::config_type *ies_config = ies::data_get_config(module_data);
    {
        bool name_recognized = true;

        if (strcmp(var_name, IES_AAPROJECTION_KEY) == 0)
            ies::config::set_aaprojection(ies_config, value);
        else if (strcmp(var_name, IES_DEBUG_KEY) == 0)
            logger->warning("The key {} is ignored", IES_DEBUG_KEY);
        else
            name_recognized = false;

        return name_recognized;
    }
}

bool get_bool(const void *arg, const char *var_name) {
    const ies::data_type *module_data = ies::data_safe_cast_const(arg);
    const ies::config::config_type *ies_config =
        ies::data_get_config(module_data);
    {
        if (strcmp(var_name, IES_AAPROJECTION_KEY) == 0)
            return ies::config::get_aaprojection(ies_config);
        else
            return false;
    }
}

bool set_double(void *arg, const char *var_name, double value) {
    ies::data_type *module_data = ies::data_safe_cast(arg);
    ies::config::config_type *ies_config = ies::data_get_config(module_data);
    {
        bool name_recognized = true;

        if (strcmp(var_name, ENKF_TRUNCATION_KEY) == 0)
            ies::config::set_truncation(ies_config, value);
        else if (strcmp(var_name, IES_MAX_STEPLENGTH_KEY) == 0)
            ies::config::set_max_steplength(ies_config, value);
        else if (strcmp(var_name, IES_MIN_STEPLENGTH_KEY) == 0)
            ies::config::set_min_steplength(ies_config, value);
        else if (strcmp(var_name, IES_DEC_STEPLENGTH_KEY) == 0)
            ies::config::set_dec_steplength(ies_config, value);
        else
            name_recognized = false;

        return name_recognized;
    }
}

double get_double(const void *arg, const char *var_name) {
    const ies::data_type *module_data = ies::data_safe_cast_const(arg);
    const ies::config::config_type *ies_config =
        ies::data_get_config(module_data);
    {
        if (strcmp(var_name, ENKF_TRUNCATION_KEY) == 0) {
            const auto &truncation = ies::config::get_truncation(ies_config);
            if (std::holds_alternative<double>(truncation))
                return std::get<double>(truncation);
            else
                return -1;
        }
        if (strcmp(var_name, IES_MAX_STEPLENGTH_KEY) == 0)
            return ies::config::get_max_steplength(ies_config);
        if (strcmp(var_name, IES_MIN_STEPLENGTH_KEY) == 0)
            return ies::config::get_min_steplength(ies_config);
        if (strcmp(var_name, IES_DEC_STEPLENGTH_KEY) == 0)
            return ies::config::get_dec_steplength(ies_config);
        return -1;
    }
}

long get_options(void *arg, long flag) {
    ies::data_type *module_data = ies::data_safe_cast(arg);
    const ies::config::config_type *ies_config =
        ies::data_get_config(module_data);
    { return ies::config::get_option_flags(ies_config); }
}

bool has_var(const void *arg, const char *var_name) {
    {
        if (strcmp(var_name, ITER_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_MAX_STEPLENGTH_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_MIN_STEPLENGTH_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_DEC_STEPLENGTH_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_INVERSION_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_LOGFILE_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_DEBUG_KEY) == 0)
            return true;
        else if (strcmp(var_name, IES_AAPROJECTION_KEY) == 0)
            return true;
        else if (strcmp(var_name, ENKF_TRUNCATION_KEY) == 0)
            return true;
        else if (strcmp(var_name, ENKF_SUBSPACE_DIMENSION_KEY) == 0)
            return true;
        else
            return false;
    }
}

void *get_ptr(const void *arg, const char *var_name) {
    const ies::data_type *module_data = ies::data_safe_cast_const(arg);
    const auto *ies_config = ies::data_get_config(module_data);
    {
        if (strcmp(var_name, IES_LOGFILE_KEY) == 0)
            return (void *)ies::config::get_logfile(ies_config);
        else
            return NULL;
    }
}
} // namespace

namespace ies {
analysis_table_type IES_ENKF = {
    .name = "IES_ENKF",
    .updateA = updateA,
    .initX = NULL,
    .init_update = init_update,
    .complete_update = NULL,
    .freef = data_free,
    .alloc = data_alloc,
    .set_int = set_int,
    .set_double = set_double,
    .set_bool = set_bool,
    .set_string = set_string,
    .get_options = get_options,
    .has_var = has_var,
    .get_int = get_int,
    .get_double = get_double,
    .get_bool = get_bool,
    .get_ptr = get_ptr,
};
}