#pragma once

#include <ert/enkf/analysis_config.hpp>
#include <ert/enkf/enkf_analysis.hpp>
#include <ert/enkf/enkf_fs.hpp>
#include <ert/enkf/ensemble_config.hpp>
#include <ert/enkf/obs_data.hpp>
#include <ert/enkf/row_scaling.hpp>
#include <ert/util/rng.hpp>
#include <iterator>
#include <stdexcept>

namespace analysis {
/**
 * Container for all data required for performing an update step.
 * Data consists of 5 matrices and a list of pairs of rowscaling and matrix.
 * objects mask describing the observations which
 * are active. In addition a flag has_observations which is used to determine wheter
 * it is possible to do an update step.
*/
class ObservationHandler
    : public std::enable_shared_from_this<ObservationHandler> {
public:
    ObservationHandler() = default;
    ObservationHandler(Eigen::VectorXd observation_values_in,
                       Eigen::VectorXd observation_errors_in,
                       const std::vector<bool> &obs_mask_in,
                       const UpdateSnapshot &update_snapshot_in)
        : observation_values(observation_values_in),
          observation_errors(observation_errors_in),
          obs_mask(std::move(obs_mask_in)),
          update_snapshot(std::move(update_snapshot_in)) {}

    Eigen::VectorXd observation_values;
    Eigen::VectorXd observation_errors;
    std::vector<bool> obs_mask;
    UpdateSnapshot update_snapshot;
};

class Parameter : public std::enable_shared_from_this<Parameter> {
public:
    std::string name;
    ActiveList active_list;

    Parameter(std::string name, const std::vector<int> &active_index = {})
        : name(name), active_index(active_index) {

        ActiveList active_list;
        if (!active_index.empty())
            for (auto &index : active_index)
                active_list.add_index(index);
        this->active_list = active_list;
    }
    void set_index_list(const std::vector<int> &active_index_list_) {
        active_index = active_index_list_;
        ActiveList active_list;
        if (!active_index.empty())
            for (auto &index : active_index)
                active_list.add_index(index);
        this->active_list = active_list;
    }
    const std::vector<int> &get_index_list() const { return active_index; }

    virtual std::string to_string() const {
        std::stringstream result;
        result << "Parameter(name='" << name << "', index_list=[";
        std::copy(active_index.begin(), active_index.end(),
                  std::ostream_iterator<int>(result, " "));
        result.seekp(-1, result.cur); // remove trailing whitespace
        result << "])";
        return result.str();
    }

private:
    std::vector<int> active_index;
};

class RowScalingParameter
    : public std::enable_shared_from_this<RowScalingParameter>,
      public Parameter {
public:
    std::shared_ptr<RowScaling> row_scaling;

    RowScalingParameter(std::string name,
                        std::shared_ptr<RowScaling> row_scaling,
                        const std::vector<int> &active_index = {})
        : Parameter(name, active_index), row_scaling(std::move(row_scaling)) {}
};

} // namespace analysis