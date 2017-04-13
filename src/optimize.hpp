#pragma once

#include "pllhead.hpp"
#include "Model.hpp"
#include "Tree_Numbers.hpp"
#include "Range.hpp"

constexpr double OPT_EPSILON        = 1.0;
constexpr double OPT_PARAM_EPSILON  = 1e-4;
constexpr double OPT_BRANCH_EPSILON = 1e-1;
constexpr double OPT_FACTR          = 1e7;
constexpr double OPT_BRLEN_MIN      = PLLMOD_OPT_MIN_BRANCH_LEN;
constexpr double OPT_BRLEN_MAX      = PLLMOD_OPT_MAX_BRANCH_LEN;
constexpr double OPT_RATE_MIN       = 1e-4;
constexpr double OPT_RATE_MAX       = 1e6;

// interface
void optimize(Model& model, 
              pll_utree_t * tree, 
              pll_partition_t * partition, 
              const Tree_Numbers& nums, 
              const bool opt_branches, 
              const bool opt_model);
void compute_and_set_empirical_frequencies(pll_partition_t * partition, Model& model);
double optimize_branch_triplet( pll_partition_t * partition, 
                                pll_utree_t * tree, 
                                const bool sliding);
