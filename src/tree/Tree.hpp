#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/pll/pll_util.hpp"
#include "core/pll/pllhead.hpp"
#include "core/pll/error.hpp"
#include "core/pll/rtree_mapper.hpp"
#include "core/raxml/Model.hpp"
#include "io/Binary.hpp"
#include "seq/MSA.hpp"
#include "tree/Tree_Numbers.hpp"
#include "util/Options.hpp"
#include "util/memory.hpp"

/* Encapsulates the pll data structures for ML computation */
class Tree {
  public:
  using Scoped_Mutex  = std::lock_guard< std::mutex >;
  using Mutex_List    = std::vector< std::mutex >;
  using partition_ptr = std::unique_ptr< pll_partition_t, partition_deleter >;
  using utree_ptr     = std::unique_ptr< pll_utree_t, utree_deleter >;

  Tree( std::string const& tree_file,
        MSA const& msa,
        raxml::Model& model,
        Options const& options,
        Memory_Footprint const& footprint = Memory_Footprint() );
  Tree( std::string const& bin_file,
        raxml::Model& model,
        Options const& options,
        Memory_Footprint const& footprint = Memory_Footprint() );
  Tree()  = default;
  ~Tree() = default;

  Tree( Tree const& other ) = delete;
  Tree( Tree&& other )      = default;

  Tree& operator=( Tree const& other ) = delete;
  Tree& operator=( Tree&& other ) = default;

  // member access
  Tree_Numbers& nums() { return nums_; }
  Tree_Numbers const& nums() const { return nums_; }
  raxml::Model& model() { return model_; }
  Options& options() { return options_; }
  auto partition() { return partition_.get(); }
  auto partition() const { return partition_.get(); }
  auto tree() { return tree_.get(); }
  rtree_mapper& mapper() { return mapper_; }
  Memory_Config& memsave() { return memsave_; }
  std::vector< unsigned int > const& branch_id() { return branch_id_; }
  unsigned int branch_id( unsigned int const node_index )
  {
    return branch_id_[ node_index ];
  }

  void ensure_clv_loaded( pll_unode_t const* const );

  double ref_tree_logl( pll_unode_t* const vroot = nullptr );

  private:
  // pll structures
  partition_ptr partition_{ nullptr, pll_partition_destroy };
  utree_ptr tree_{ nullptr, utree_destroy };

  // Object holding memory saving related structures
  Memory_Config memsave_;

  // tree related
  Tree_Numbers nums_;
  // map from node indexes to branch ID consistent with jplace standard
  std::vector< unsigned int > branch_id_;

  // epa related classes
  MSA ref_msa_;
  raxml::Model model_;
  Options options_;
  Binary binary_;
  rtree_mapper mapper_;

  // thread safety
  Mutex_List locks_;
};
