/*
   See the file README.obs for ducumentation of the varios datatypes
   involved with observations/measurement/+++.
*/

#include <cmath>
#include <pthread.h>
#include <stdlib.h>

#include <Eigen/Dense>
#include <algorithm>
#include <vector>

#include <ert/util/hash.h>
#include <ert/util/type_vector_functions.h>
#include <ert/util/vector.h>

#include <ert/enkf/meas_data.hpp>

struct meas_data_struct {
    int active_ens_size;
    vector_type *data;
    pthread_mutex_t data_mutex;
    hash_type *blocks;
    std::vector<bool> ens_mask;
};

struct meas_block_struct {
    int active_ens_size;
    int obs_size;
    int ens_stride;
    int obs_stride;
    int data_size;
    char *obs_key;
    double *data;
    bool *active;
    bool stat_calculated;
    std::vector<bool> ens_mask;
    int_vector_type *index_map;
};

/*
   Observe that meas_block instance must be allocated with a correct
   value for obs_size; it can not grow during use, and it does also
   not count the number of elements added.

   Observe that the input argument @obs_size should be the total size
   of the observation; if parts of the observation have been excluded
   due to local analysis it should still be included in the @obs_size
   value.
*/

namespace {
int_vector_type *
bool_vector_to_active_index_list(const std::vector<bool> &bool_vector) {
    int_vector_type *index_list = int_vector_alloc(bool_vector.size(), -1);
    int active_index = 0;
    for (int i = 0; i < bool_vector.size(); i++) {
        if (bool_vector[i]) {
            int_vector_iset(index_list, i, active_index);
            active_index++;
        }
    }
    return index_list;
}
} // namespace

meas_block_type *meas_block_alloc(const char *obs_key,
                                  const std::vector<bool> &ens_mask,
                                  int obs_size) {
    auto meas_block = new meas_block_type;
    meas_block->active_ens_size =
        std::count(ens_mask.begin(), ens_mask.end(), true);
    meas_block->ens_mask = ens_mask;
    meas_block->obs_size = obs_size;
    meas_block->obs_key = util_alloc_string_copy(obs_key);
    meas_block->data = (double *)util_calloc(
        (meas_block->active_ens_size + 2) * obs_size, sizeof *meas_block->data);
    meas_block->active =
        (bool *)util_calloc(obs_size, sizeof *meas_block->active);
    meas_block->ens_stride = 1;
    meas_block->obs_stride = meas_block->active_ens_size + 2;
    meas_block->data_size = (meas_block->active_ens_size + 2) * obs_size;
    meas_block->index_map =
        bool_vector_to_active_index_list(meas_block->ens_mask);
    {
        int i;
        for (i = 0; i < obs_size; i++)
            meas_block->active[i] = false;
    }
    meas_block->stat_calculated = false;
    return meas_block;
}

void meas_block_free(meas_block_type *meas_block) {
    free(meas_block->obs_key);
    free(meas_block->data);
    free(meas_block->active);
    int_vector_free(meas_block->index_map);
    delete meas_block;
}

static void meas_block_free__(void *arg) {
    auto meas_block = static_cast<meas_block_type *>(arg);
    meas_block_free(meas_block);
}

static void meas_block_initS(const meas_block_type *meas_block,
                             Eigen::MatrixXd &S, int *__obs_offset) {
    int obs_offset = *__obs_offset;
    for (int iobs = 0; iobs < meas_block->obs_size; iobs++) {
        if (meas_block->active[iobs]) {
            for (int iens = 0; iens < meas_block->active_ens_size; iens++) {
                int obs_index = iens * meas_block->ens_stride +
                                iobs * meas_block->obs_stride;

                S(obs_offset, iens) = meas_block->data[obs_index];
            }
            obs_offset++;
        }
    }
    *__obs_offset = obs_offset;
}

void meas_block_calculate_ens_stats(meas_block_type *meas_block) {
    for (int iobs = 0; iobs < meas_block->obs_size; iobs++) {
        if (meas_block->active[iobs]) {
            double M1 = 0;
            double M2 = 0;
            for (int iens = 0; iens < meas_block->active_ens_size; iens++) {
                int index = iens * meas_block->ens_stride +
                            iobs * meas_block->obs_stride;
                M1 += meas_block->data[index];
                M2 += meas_block->data[index] * meas_block->data[index];
            }
            int mean_index =
                (meas_block->active_ens_size + 0) * meas_block->ens_stride +
                iobs * meas_block->obs_stride;
            int std_index =
                (meas_block->active_ens_size + 1) * meas_block->ens_stride +
                iobs * meas_block->obs_stride;
            double mean = M1 / meas_block->active_ens_size;
            double var = M2 / meas_block->active_ens_size - mean * mean;
            meas_block->data[mean_index] = mean;
            meas_block->data[std_index] = sqrt(std::max(0.0, var));
        }
    }
    meas_block->stat_calculated = true;
}

static void meas_block_assert_ens_stat(meas_block_type *meas_block) {
    if (!meas_block->stat_calculated)
        meas_block_calculate_ens_stats(meas_block);
}

static void meas_block_assert_iens_active(const meas_block_type *meas_block,
                                          int iens) {
    if (!meas_block->ens_mask[iens])
        util_abort(
            "%s: fatal error - trying to access inactive ensemble member:%d \n",
            __func__, iens);
}

void meas_block_iset(meas_block_type *meas_block, int iens, int iobs,
                     double value) {
    meas_block_assert_iens_active(meas_block, iens);
    {
        int active_iens = int_vector_iget(meas_block->index_map, iens);
        int index = active_iens * meas_block->ens_stride +
                    iobs * meas_block->obs_stride;
        meas_block->data[index] = value;
        if (!meas_block->active[iobs])
            meas_block->active[iobs] = true;

        meas_block->stat_calculated = false;
    }
}

double meas_block_iget(const meas_block_type *meas_block, int iens, int iobs) {
    meas_block_assert_iens_active(meas_block, iens);
    {
        int active_iens = int_vector_iget(meas_block->index_map, iens);
        int index = active_iens * meas_block->ens_stride +
                    iobs * meas_block->obs_stride;
        return meas_block->data[index];
    }
}

static int meas_block_get_active_obs_size(const meas_block_type *meas_block) {
    int obs_size = 0;
    int i;

    for (i = 0; i < meas_block->obs_size; i++)
        if (meas_block->active[i])
            obs_size++;

    return obs_size;
}

double meas_block_iget_ens_std(meas_block_type *meas_block, int iobs) {
    meas_block_assert_ens_stat(meas_block);
    {
        int std_index =
            (meas_block->active_ens_size + 1) * meas_block->ens_stride +
            iobs * meas_block->obs_stride;
        return meas_block->data[std_index];
    }
}

double meas_block_iget_ens_mean(meas_block_type *meas_block, int iobs) {
    meas_block_assert_ens_stat(meas_block);
    {
        int mean_index = meas_block->active_ens_size * meas_block->ens_stride +
                         iobs * meas_block->obs_stride;
        return meas_block->data[mean_index];
    }
}

bool meas_block_iget_active(const meas_block_type *meas_block, int iobs) {
    return meas_block->active[iobs];
}

void meas_block_deactivate(meas_block_type *meas_block, int iobs) {
    if (meas_block->active[iobs])
        meas_block->active[iobs] = false;
    meas_block->stat_calculated = false;
}

int meas_block_get_total_obs_size(const meas_block_type *meas_block) {
    return meas_block->obs_size;
}

int meas_block_get_active_ens_size(const meas_block_type *meas_block) {
    return meas_block->active_ens_size;
}

int meas_block_get_total_ens_size(const meas_block_type *meas_block) {
    return meas_block->ens_mask.size();
}

meas_data_type *meas_data_alloc(const std::vector<bool> &ens_mask) {
    auto meas = new meas_data_type;

    meas->data = vector_alloc_new();
    meas->blocks = hash_alloc();
    meas->ens_mask = ens_mask;
    meas->active_ens_size = std::count(ens_mask.begin(), ens_mask.end(), true);
    pthread_mutex_init(&meas->data_mutex, NULL);

    return meas;
}

void meas_data_free(meas_data_type *matrix) {
    vector_free(matrix->data);
    hash_free(matrix->blocks);
    delete matrix;
}

/*
   The obs_key is not alone unique over different report steps.
*/
static char *meas_data_alloc_key(const char *obs_key, int report_step) {
    return util_alloc_sprintf("%s-%d", obs_key, report_step);
}

/*
   The code actually adding new blocks to the vector must be run in single-thread mode.
*/

meas_block_type *meas_data_add_block(meas_data_type *matrix,
                                     const char *obs_key, int report_step,
                                     int obs_size) {
    char *lookup_key = meas_data_alloc_key(obs_key, report_step);
    pthread_mutex_lock(&matrix->data_mutex);
    {
        if (!hash_has_key(matrix->blocks, lookup_key)) {
            meas_block_type *new_block =
                meas_block_alloc(obs_key, matrix->ens_mask, obs_size);
            vector_append_owned_ref(matrix->data, new_block, meas_block_free__);
            hash_insert_ref(matrix->blocks, lookup_key, new_block);
        }
    }
    pthread_mutex_unlock(&matrix->data_mutex);
    free(lookup_key);
    return (meas_block_type *)vector_get_last(matrix->data);
}

meas_block_type *meas_data_iget_block(const meas_data_type *matrix,
                                      int block_nr) {
    return (meas_block_type *)vector_iget(matrix->data, block_nr);
}

int meas_data_get_active_obs_size(const meas_data_type *matrix) {
    int obs_size = 0;

    for (int block_nr = 0; block_nr < vector_get_size(matrix->data);
         block_nr++) {
        const meas_block_type *meas_block =
            (const meas_block_type *)vector_iget_const(matrix->data, block_nr);
        obs_size += meas_block_get_active_obs_size(meas_block);
    }

    return obs_size;
}

Eigen::MatrixXd meas_data_makeS(const meas_data_type *matrix) {
    int obs_offset = 0;
    Eigen::MatrixXd S = Eigen::MatrixXd::Zero(
        meas_data_get_active_obs_size(matrix), matrix->active_ens_size);
    if (S.rows() > 0 && S.cols() > 0) {
        for (int block_nr = 0; block_nr < vector_get_size(matrix->data);
             block_nr++) {
            const meas_block_type *meas_block =
                (const meas_block_type *)vector_iget_const(matrix->data,
                                                           block_nr);
            meas_block_initS(meas_block, S, &obs_offset);
        }
    }
    return S;
}

int meas_data_get_active_ens_size(const meas_data_type *meas_data) {
    return meas_data->active_ens_size;
}
