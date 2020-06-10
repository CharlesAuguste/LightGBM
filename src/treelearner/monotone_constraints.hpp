/*!
 * Copyright (c) 2020 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for
 * license information.
 */
#ifndef LIGHTGBM_TREELEARNER_MONOTONE_CONSTRAINTS_HPP_
#define LIGHTGBM_TREELEARNER_MONOTONE_CONSTRAINTS_HPP_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "split_info.hpp"

namespace LightGBM {

class LeafConstraintsBase;

struct BasicConstraint {
  double min = -std::numeric_limits<double>::max();
  double max = std::numeric_limits<double>::max();

  BasicConstraint(double min, double max) : min(min), max(max) {}

  BasicConstraint() = default;
};

struct FeatureConstraint {
  virtual void InitCumulativeConstraints(bool) {};
  virtual void Update(int) {};
  virtual BasicConstraint LeftToBasicConstraint() const = 0;
  virtual BasicConstraint RightToBasicConstraint() const = 0;
  virtual bool ConstraintDifferentDependingOnThreshold() const = 0;
};

struct ConstraintEntry {
  virtual void Reset() = 0;
  virtual void UpdateMin(double new_min) = 0;
  virtual void UpdateMax(double new_max) = 0;
  virtual bool UpdateMinAndReturnBoolIfChanged(double new_min) = 0;
  virtual bool UpdateMaxAndReturnBoolIfChanged(double new_max) = 0;
  virtual ConstraintEntry *clone() const = 0;

  virtual void RecomputeConstraintsIfNeeded(LeafConstraintsBase *, int, int,
                                            uint32_t) {};

  virtual FeatureConstraint *GetFeatureConstraint(int feature_index) = 0;
};

// used by both BasicLeafConstraints and IntermediateLeafConstraints
struct BasicConstraintEntry : ConstraintEntry,
                              FeatureConstraint,
                              BasicConstraint {

  bool ConstraintDifferentDependingOnThreshold() const final { return false; }

  BasicConstraintEntry *clone() const final {
    return new BasicConstraintEntry(*this);
  };

  void Reset() final {
    min = -std::numeric_limits<double>::max();
    max = std::numeric_limits<double>::max();
  }

  void UpdateMin(double new_min) final { min = std::max(new_min, min); }

  void UpdateMax(double new_max) final { max = std::min(new_max, max); }

  bool UpdateMinAndReturnBoolIfChanged(double new_min) final {
    if (new_min > min) {
      min = new_min;
      return true;
    }
    return false;
  }

  bool UpdateMaxAndReturnBoolIfChanged(double new_max) final {
    if (new_max < max) {
      max = new_max;
      return true;
    }
    return false;
  }

  BasicConstraint LeftToBasicConstraint() const final { return *this; }

  BasicConstraint RightToBasicConstraint() const final { return *this; }

  FeatureConstraint *GetFeatureConstraint(int) final { return this; }
};

class LeafConstraintsBase {
 public:
  virtual ~LeafConstraintsBase() {}
  virtual const ConstraintEntry* Get(int leaf_idx) = 0;
  virtual FeatureConstraint* GetFeatureConstraint(int leaf_idx, int feature_index) = 0;
  virtual void Reset() = 0;
  virtual void BeforeSplit(int leaf, int new_leaf,
                           int8_t monotone_type) = 0;
  virtual std::vector<int> Update(
      bool is_numerical_split,
      int leaf, int new_leaf, int8_t monotone_type, double right_output,
      double left_output, int split_feature, const SplitInfo& split_info,
      const std::vector<SplitInfo>& best_split_per_leaf) = 0;

  virtual void RecomputeConstraintsIfNeeded(
      LeafConstraintsBase *constraints_,
      int feature_for_constraint, int leaf_idx, uint32_t it_end) = 0;

  inline static LeafConstraintsBase* Create(const Config* config, int num_leaves, int num_features);

  double ComputeMonotoneSplitGainPenalty(int leaf_index, double penalization) {
    int depth = tree_->leaf_depth(leaf_index);
    if (penalization >= depth + 1.) {
      return kEpsilon;
    }
    if (penalization <= 1.) {
      return 1. - penalization / pow(2., depth) + kEpsilon;
    }
    return 1. - pow(2, penalization - 1. - depth) + kEpsilon;
  }

  void ShareTreePointer(const Tree* tree) {
    tree_ = tree;
  }

 protected:
  const Tree* tree_;
};

class BasicLeafConstraints : public LeafConstraintsBase {
 public:
  explicit BasicLeafConstraints(int num_leaves) : num_leaves_(num_leaves) {
    for (int i = 0; i < num_leaves; i++) {
      entries_.push_back(new BasicConstraintEntry());
    }
  }

  void Reset() override {
    for (auto entry : entries_) {
      entry->Reset();
    }
  }

  void RecomputeConstraintsIfNeeded(
      LeafConstraintsBase* constraints_,
      int feature_for_constraint, int leaf_idx, uint32_t it_end) {
    entries_[~leaf_idx]->RecomputeConstraintsIfNeeded(constraints_, feature_for_constraint, leaf_idx, it_end);
  }

  void BeforeSplit(int, int, int8_t) override {}

  std::vector<int> Update(bool is_numerical_split, int leaf, int new_leaf,
                          int8_t monotone_type, double right_output,
                          double left_output, int, const SplitInfo& ,
                          const std::vector<SplitInfo>&) override {
    entries_[new_leaf] = entries_[leaf]->clone();
    if (is_numerical_split) {
      double mid = (left_output + right_output) / 2.0f;
      if (monotone_type < 0) {
        entries_[leaf]->UpdateMin(mid);
        entries_[new_leaf]->UpdateMax(mid);
      } else if (monotone_type > 0) {
        entries_[leaf]->UpdateMax(mid);
        entries_[new_leaf]->UpdateMin(mid);
      }
    }
    return std::vector<int>();
  }

  const ConstraintEntry* Get(int leaf_idx) override { return entries_[leaf_idx]; }

  FeatureConstraint* GetFeatureConstraint(int leaf_idx, int feature_index) {
    return entries_[leaf_idx]->GetFeatureConstraint(feature_index);
  }

 protected:
  int num_leaves_;
  std::vector<ConstraintEntry*> entries_;
};

class IntermediateLeafConstraints : public BasicLeafConstraints {
 public:
  explicit IntermediateLeafConstraints(const Config* config, int num_leaves)
      : BasicLeafConstraints(num_leaves), config_(config) {
    leaf_is_in_monotone_subtree_.resize(num_leaves_, false);
    node_parent_.resize(num_leaves_ - 1, -1);
    leaves_to_update_.reserve(num_leaves_);
  }

  void Reset() override {
    BasicLeafConstraints::Reset();
    std::fill_n(leaf_is_in_monotone_subtree_.begin(), num_leaves_, false);
    std::fill_n(node_parent_.begin(), num_leaves_ - 1, -1);
    leaves_to_update_.clear();
  }

  void BeforeSplit(int leaf, int new_leaf,
                   int8_t monotone_type) override {
    if (monotone_type != 0 || leaf_is_in_monotone_subtree_[leaf]) {
      leaf_is_in_monotone_subtree_[leaf] = true;
      leaf_is_in_monotone_subtree_[new_leaf] = true;
    }
#ifdef DEBUG
    CHECK_GE(new_leaf - 1, 0);
    CHECK_LT(static_cast<size_t>(new_leaf - 1), node_parent_.size());
#endif
    node_parent_[new_leaf - 1] = tree_->leaf_parent(leaf);
  }

  void UpdateConstraintsWithOutputs(bool is_numerical_split, int leaf,
                                    int new_leaf, int8_t monotone_type,
                                    double right_output, double left_output) {
    entries_[new_leaf] = entries_[leaf]->clone();
    if (is_numerical_split) {
      if (monotone_type < 0) {
        entries_[leaf]->UpdateMin(right_output);
        entries_[new_leaf]->UpdateMax(left_output);
      } else if (monotone_type > 0) {
        entries_[leaf]->UpdateMax(right_output);
        entries_[new_leaf]->UpdateMin(left_output);
      }
    }
  }

  std::vector<int> Update(bool is_numerical_split, int leaf,
                          int new_leaf, int8_t monotone_type,
                          double right_output, double left_output,
                          int split_feature, const SplitInfo& split_info,
                          const std::vector<SplitInfo>& best_split_per_leaf) final {
    leaves_to_update_.clear();
    if (leaf_is_in_monotone_subtree_[leaf]) {
      UpdateConstraintsWithOutputs(is_numerical_split, leaf, new_leaf,
                                   monotone_type, right_output, left_output);

      // Initialize variables to store information while going up the tree
      int depth = tree_->leaf_depth(new_leaf) - 1;

      std::vector<int> features_of_splits_going_up_from_original_leaf;
      std::vector<uint32_t> thresholds_of_splits_going_up_from_original_leaf;
      std::vector<bool> was_original_leaf_right_child_of_split;

      features_of_splits_going_up_from_original_leaf.reserve(depth);
      thresholds_of_splits_going_up_from_original_leaf.reserve(depth);
      was_original_leaf_right_child_of_split.reserve(depth);

      GoUpToFindLeavesToUpdate(tree_->leaf_parent(new_leaf),
                               &features_of_splits_going_up_from_original_leaf,
                               &thresholds_of_splits_going_up_from_original_leaf,
                               &was_original_leaf_right_child_of_split,
                               split_feature, split_info, split_info.threshold,
                               best_split_per_leaf);
    }
    return leaves_to_update_;
  }

  bool OppositeChildShouldBeUpdated(
      bool is_split_numerical,
      const std::vector<int>& features_of_splits_going_up_from_original_leaf,
      int inner_feature,
      const std::vector<bool>& was_original_leaf_right_child_of_split,
      bool is_in_right_child) {

    // if the split is categorical, it is not handled by this optimisation,
    // so the code will have to go down in the other child subtree to see if
    // there are leaves to update
    // even though it may sometimes be unnecessary
    if (is_split_numerical) {
      // only branches containing leaves that are contiguous to the original
      // leaf need to be updated
      // therefore, for the same feature, there is no use going down from the
      // second time going up on the right (or on the left)
      for (size_t split_idx = 0;
           split_idx < features_of_splits_going_up_from_original_leaf.size();
           ++split_idx) {
        if (features_of_splits_going_up_from_original_leaf[split_idx] ==
                inner_feature &&
            (was_original_leaf_right_child_of_split[split_idx] ==
             is_in_right_child)) {
          return false;
        }
      }
      return true;
    }
    else {
      return false;
    }
  }

  // Recursive function that goes up the tree, and then down to find leaves that
  // have constraints to be updated
  void GoUpToFindLeavesToUpdate(
      int node_idx,
      std::vector<int>* features_of_splits_going_up_from_original_leaf,
      std::vector<uint32_t>* thresholds_of_splits_going_up_from_original_leaf,
      std::vector<bool>* was_original_leaf_right_child_of_split,
      int split_feature, const SplitInfo& split_info, uint32_t split_threshold,
      const std::vector<SplitInfo>& best_split_per_leaf) {
#ifdef DEBUG
    CHECK_GE(node_idx, 0);
    CHECK_LT(static_cast<size_t>(node_idx), node_parent_.size());
#endif
    int parent_idx = node_parent_[node_idx];
    // if not at the root
    if (parent_idx != -1) {
      int inner_feature = tree_->split_feature_inner(parent_idx);
      int feature = tree_->split_feature(parent_idx);
      int8_t monotone_type = config_->monotone_constraints[feature];
      bool is_in_right_child = tree_->right_child(parent_idx) == node_idx;
      bool is_split_numerical = tree_->IsNumericalSplit(parent_idx);

      // this is just an optimisation not to waste time going down in subtrees
      // where there won't be any leaf to update
      bool opposite_child_should_be_updated = OppositeChildShouldBeUpdated(
          is_split_numerical, *features_of_splits_going_up_from_original_leaf,
          inner_feature, *was_original_leaf_right_child_of_split,
          is_in_right_child);

      if (opposite_child_should_be_updated) {
        // if there is no monotone constraint on a split,
        // then there is no relationship between its left and right leaves' values
        if (monotone_type != 0) {
          // these variables correspond to the current split we encounter going
          // up the tree
          int left_child_idx = tree_->left_child(parent_idx);
          int right_child_idx = tree_->right_child(parent_idx);
          bool left_child_is_curr_idx = (left_child_idx == node_idx);
          int opposite_child_idx =
              (left_child_is_curr_idx) ? right_child_idx : left_child_idx;
          bool update_max_constraints_in_opposite_child_leaves =
              (monotone_type < 0) ? left_child_is_curr_idx
                                  : !left_child_is_curr_idx;

          // the opposite child needs to be updated
          // so the code needs to go down in the the opposite child
          // to see which leaves' constraints need to be updated
          GoDownToFindLeavesToUpdate(
              opposite_child_idx,
              *features_of_splits_going_up_from_original_leaf,
              *thresholds_of_splits_going_up_from_original_leaf,
              *was_original_leaf_right_child_of_split,
              update_max_constraints_in_opposite_child_leaves, split_feature,
              split_info, true, true, split_threshold, best_split_per_leaf);
        }

        // if opposite_child_should_be_updated, then it means the path to come up there was relevant,
        // i.e. that it will be helpful going down to determine which leaf
        // is actually contiguous to the original 2 leaves and should be updated
        // so the variables associated with the split need to be recorded
        was_original_leaf_right_child_of_split->push_back(
            tree_->right_child(parent_idx) == node_idx);
        thresholds_of_splits_going_up_from_original_leaf->push_back(
            tree_->threshold_in_bin(parent_idx));
        features_of_splits_going_up_from_original_leaf->push_back(
            tree_->split_feature_inner(parent_idx));
      }

      // since current node is not the root, keep going up
      GoUpToFindLeavesToUpdate(
          parent_idx, features_of_splits_going_up_from_original_leaf,
          thresholds_of_splits_going_up_from_original_leaf,
          was_original_leaf_right_child_of_split, split_feature, split_info,
          split_threshold, best_split_per_leaf);
    }
  }

  void GoDownToFindLeavesToUpdate(
      int node_idx,
      const std::vector<int>& features_of_splits_going_up_from_original_leaf,
      const std::vector<uint32_t>&
          thresholds_of_splits_going_up_from_original_leaf,
      const std::vector<bool>& was_original_leaf_right_child_of_split,
      bool update_max_constraints, int split_feature,
      const SplitInfo& split_info, bool use_left_leaf, bool use_right_leaf,
      uint32_t split_threshold,
      const std::vector<SplitInfo>& best_split_per_leaf) {
    // if leaf
    if (node_idx < 0) {
      int leaf_idx = ~node_idx;

      // splits that are not to be used shall not be updated,
      // included leaf at max depth
      if (best_split_per_leaf[leaf_idx].gain == kMinScore) {
        return;
      }

      std::pair<double, double> min_max_constraints;
      bool something_changed = false;
      // if the current leaf is contiguous with both the new right leaf and the new left leaf
      // then it may need to be greater than the max of the 2 or smaller than the min of the 2
      // otherwise, if the current leaf is contiguous with only one of the 2 new leaves,
      // then it may need to be greater or smaller than it
      if (use_right_leaf && use_left_leaf) {
        min_max_constraints =
            std::minmax(split_info.right_output, split_info.left_output);
      } else if (use_right_leaf && !use_left_leaf) {
        min_max_constraints = std::pair<double, double>(
            split_info.right_output, split_info.right_output);
      } else {
        min_max_constraints = std::pair<double, double>(split_info.left_output,
                                                        split_info.left_output);
      }

#ifdef DEBUG
      if (update_max_constraints) {
        CHECK_GE(min_max_constraints.first, tree_->LeafOutput(leaf_idx));
      } else {
        CHECK_LE(min_max_constraints.second, tree_->LeafOutput(leaf_idx));
      }
#endif
      // depending on which split made the current leaf and the original leaves contiguous,
      // either the min constraint or the max constraint of the current leaf need to be updated
      if (!update_max_constraints) {
        something_changed = entries_[leaf_idx]->UpdateMinAndReturnBoolIfChanged(
            min_max_constraints.second);
      } else {
        something_changed = entries_[leaf_idx]->UpdateMaxAndReturnBoolIfChanged(
            min_max_constraints.first);
      }
      // If constraints were not updated, then there is no need to update the leaf
      if (!something_changed) {
        return;
      }
      leaves_to_update_.push_back(leaf_idx);

    } else {  // if node
      // check if the children are contiguous with the original leaf
      std::pair<bool, bool> keep_going_left_right = ShouldKeepGoingLeftRight(
          node_idx, features_of_splits_going_up_from_original_leaf,
          thresholds_of_splits_going_up_from_original_leaf,
          was_original_leaf_right_child_of_split);
      int inner_feature = tree_->split_feature_inner(node_idx);
      uint32_t threshold = tree_->threshold_in_bin(node_idx);
      bool is_split_numerical = tree_->IsNumericalSplit(node_idx);
      bool use_left_leaf_for_update_right = true;
      bool use_right_leaf_for_update_left = true;
      // if the split is on the same feature (categorical variables not supported)
      // then depending on the threshold,
      // the current left child may not be contiguous with the original right leaf,
      // or the current right child may not be contiguous with the original left leaf
      if (is_split_numerical && inner_feature == split_feature) {
        if (threshold >= split_threshold) {
          use_left_leaf_for_update_right = false;
        }
        if (threshold <= split_threshold) {
          use_right_leaf_for_update_left = false;
        }
      }

      // go down left
      if (keep_going_left_right.first) {
        GoDownToFindLeavesToUpdate(
            tree_->left_child(node_idx),
            features_of_splits_going_up_from_original_leaf,
            thresholds_of_splits_going_up_from_original_leaf,
            was_original_leaf_right_child_of_split, update_max_constraints,
            split_feature, split_info, use_left_leaf,
            use_right_leaf_for_update_left && use_right_leaf, split_threshold,
            best_split_per_leaf);
      }
      // go down right
      if (keep_going_left_right.second) {
        GoDownToFindLeavesToUpdate(
            tree_->right_child(node_idx),
            features_of_splits_going_up_from_original_leaf,
            thresholds_of_splits_going_up_from_original_leaf,
            was_original_leaf_right_child_of_split, update_max_constraints,
            split_feature, split_info,
            use_left_leaf_for_update_right && use_left_leaf, use_right_leaf,
            split_threshold, best_split_per_leaf);
      }
    }
  }

  std::pair<bool, bool> ShouldKeepGoingLeftRight(
      int node_idx,
      const std::vector<int>& features_of_splits_going_up_from_original_leaf,
      const std::vector<uint32_t>&
          thresholds_of_splits_going_up_from_original_leaf,
      const std::vector<bool>& was_original_leaf_right_child_of_split) {
    int inner_feature = tree_->split_feature_inner(node_idx);
    uint32_t threshold = tree_->threshold_in_bin(node_idx);
    bool is_split_numerical = tree_->IsNumericalSplit(node_idx);

    bool keep_going_right = true;
    bool keep_going_left = true;
    // left and right nodes are checked to find out if they are contiguous with
    // the original leaves if so the algorithm should keep going down these nodes
    // to update constraints
    if (is_split_numerical) {
      for (size_t i = 0;
           i < features_of_splits_going_up_from_original_leaf.size(); ++i) {
        if (features_of_splits_going_up_from_original_leaf[i] ==
            inner_feature) {
          if (threshold >=
                  thresholds_of_splits_going_up_from_original_leaf[i] &&
              !was_original_leaf_right_child_of_split[i]) {
            keep_going_right = false;
            if (!keep_going_left) {
              break;
            }
          }
          if (threshold <=
                  thresholds_of_splits_going_up_from_original_leaf[i] &&
              was_original_leaf_right_child_of_split[i]) {
            keep_going_left = false;
            if (!keep_going_right) {
              break;
            }
          }
        }
      }
    }
    return std::pair<bool, bool>(keep_going_left, keep_going_right);
  }

 protected:
  const Config* config_;
  std::vector<int> leaves_to_update_;
  // add parent node information
  std::vector<int> node_parent_;
  // Keeps track of the monotone splits above the leaf
  std::vector<bool> leaf_is_in_monotone_subtree_;
};

LeafConstraintsBase* LeafConstraintsBase::Create(const Config* config,
                                                 int num_leaves, int num_features) {
  if (config->monotone_constraints_method == "intermediate") {
    return new IntermediateLeafConstraints(config, num_leaves);
  }
  return new BasicLeafConstraints(num_leaves);
}

}  // namespace LightGBM
#endif  // LIGHTGBM_TREELEARNER_MONOTONE_CONSTRAINTS_HPP_
