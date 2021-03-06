// This file is part of CAF, the C++ Actor Framework. See the file LICENSE in
// the main distribution directory for license terms and copyright or visit
// https://github.com/actor-framework/actor-framework/blob/master/LICENSE.

#pragma once

#include <tuple>
#include <cstddef>

#include "caf/downstream_manager.hpp"
#include "caf/logger.hpp"
#include "caf/outbound_path.hpp"
#include "caf/sec.hpp"
#include "caf/stream.hpp"
#include "caf/stream_slot.hpp"

#include "caf/detail/type_list.hpp"
#include "caf/detail/unordered_flat_map.hpp"

namespace caf {
namespace detail {

/// Utility function for repeating `x` for a given template parameter pack.
template <class T, class U>
U pack_repeat(U x) {
  return x;
}

template <class Iter>
class ptr_array_initializer {
public:
  ptr_array_initializer(Iter first) : i_(first) {
    // nop
  }

  void operator()() {
    // end of recursion
  }

  template <class T, class... Ts>
  void operator()(T& x, Ts&... xs) {
    *i_ = &x;
    ++i_;
    (*this)(xs...);
  }

private:
  Iter i_;
};

struct downstream_manager_selector {
  downstream_manager* operator()(const message&) {
    return nullptr;
  }

  template <class T, class... Ts>
  downstream_manager* operator()(const message& msg, T& x, Ts&... xs) {
    if (msg.size() > 1
        && msg.type_at(0) == type_id_v<stream<typename T::value_type>>)
      return &x;
    return (*this)(msg, xs...);
  }
};

template <size_t I, size_t E>
struct init_ptr_array {
  template <class... Ts>
  static void apply(downstream_manager* (&xs)[E], std::tuple<Ts...>& ys)  {
    xs[I] = &std::get<I>(ys);
    init_ptr_array<I + 1, E>::apply(xs, ys);
  }
};

template <size_t I>
struct init_ptr_array<I, I> {
  template <class... Ts>
  static void apply(downstream_manager* (&)[I], std::tuple<Ts...>&)  {
    // nop
  }
};

} // namespace detail

/// A downstream manager that delegates to any number of sub-managers.
template <class T, class... Ts>
class fused_downstream_manager : public downstream_manager {
public:
  // -- member and nested types ------------------------------------------------

  /// Base type.
  using super = downstream_manager;

  /// A tuple holding all nested managers.
  using nested_managers = std::tuple<T, Ts...>;

  /// Pointer to an outbound path.
  using typename super::path_ptr;

  /// Unique pointer to an outbound path.
  using typename super::unique_path_ptr;

  // Lists all template parameters `[T, Ts...]`;
  using param_list = detail::type_list<T, Ts...>;

  /// State held for each slot.
  struct non_owning_ptr {
    path_ptr ptr;
    downstream_manager* owner;
  };

  /// Maps slots to path and nested managers.
  using map_type = detail::unordered_flat_map<stream_slot, non_owning_ptr>;

  /// Maps slots to paths that haven't a managers assigned yet.
  using unassigned_map_type = detail::unordered_flat_map<stream_slot,
                                                         unique_path_ptr>;

  // -- constructors, destructors, and assignment operators --------------------

  fused_downstream_manager(stream_manager* parent)
      : super(parent),
        nested_(parent, detail::pack_repeat<Ts>(parent)...) {
    detail::init_ptr_array<0, sizeof...(Ts) + 1>::apply(ptrs_, nested_);
  }

  // -- properties -------------------------------------------------------------

  template <class U>
  U& get() {
    return std::get<U>(nested_);
  }

  template <class U>
  const U& get() const {
    return std::get<U>(nested_);
  }

  /// Requires a previous call to `add_path` for given slot.
  template <class U>
  void assign(stream_slot slot) {
    // Fetch pointer from the unassigned paths.
    auto i = unassigned_paths_.find(slot);
    if (i == unassigned_paths_.end()) {
      CAF_LOG_ERROR("cannot assign nested manager to unknown slot");
      return;
    }
    // Error or not, remove entry from unassigned_paths_ before leaving.
    auto cleanup = detail::make_scope_guard([&] {
      unassigned_paths_.erase(i);
    });
    // Transfer ownership to nested manager.
    auto ptr = i->second.get();
    CAF_ASSERT(ptr != nullptr);
    auto owner = &get<U>();
    if (!owner->insert_path(std::move(i->second))) {
      CAF_LOG_ERROR("slot exists as unassigned and assigned");
      return;
    }
    // Store owner and path in our map.
    auto er = paths_.emplace(slot, non_owning_ptr{ptr, owner});
    if (!er.second) {
      CAF_LOG_ERROR("slot already mapped");
      owner->remove_path(slot, sec::invalid_stream_state, false);
      return;
    }
  }

  // -- overridden functions ---------------------------------------------------

  bool terminal() const noexcept override {
    return false;
  }

  size_t num_paths() const noexcept override {
    return paths_.size();
  }

  bool remove_path(stream_slot slot, error reason,
                   bool silent) noexcept override {
    CAF_LOG_TRACE(CAF_ARG(slot) << CAF_ARG(reason) << CAF_ARG(silent));
    auto i = paths_.find(slot);
    if (i == paths_.end())
      return false;
    auto owner = i->second.owner;
    paths_.erase(i);
    return owner->remove_path(slot, std::move(reason), silent);
  }

  path_ptr path(stream_slot slot) noexcept override {
    auto i = paths_.find(slot);
    if (i == paths_.end())
      return nullptr;
    return i->second.ptr;
  }

  using downstream_manager::close;

  void close() override {
    CAF_LOG_TRACE(CAF_ARG(paths_));
    for (auto ptr : ptrs_)
      ptr->close();
    paths_.clear();
  }

  void abort(error reason) override {
    CAF_LOG_TRACE(CAF_ARG(paths_));
    for (auto ptr : ptrs_)
      ptr->abort(reason);
    paths_.clear();
  }

  void emit_batches() override {
    CAF_LOG_TRACE("");
    for (auto ptr : ptrs_)
      ptr->emit_batches();
  }

  void force_emit_batches() override {
    CAF_LOG_TRACE("");
    for (auto ptr : ptrs_)
      ptr->force_emit_batches();
  }

  size_t capacity() const noexcept override {
    // Get the minimum of all available capacities.
    size_t result = std::numeric_limits<size_t>::max();
    for (auto ptr : ptrs_)
      result = std::min(result, ptr->capacity());
    return result;
  }

  size_t buffered() const noexcept override {
    // Get the maximum of all available buffer sizes.
    size_t result = 0;
    for (auto ptr : ptrs_)
      result = std::max(result, ptr->buffered());
    return result;
  }

  size_t buffered(stream_slot slot) const noexcept override {
    // We don't know which nested manager stores this path. Only one will give a
    // valid answer, though. Everyone else always responds with 0. Hence, we can
    // simply call all managers and sum up the results.
    size_t result = 0;
    for (auto ptr : ptrs_)
      result += ptr->buffered(slot);
    return result;
  }

  void clear_paths() override {
    CAF_LOG_TRACE("");
    for (auto ptr : ptrs_)
      ptr->clear_paths();
    paths_.clear();
  }

protected:
  bool insert_path(unique_path_ptr ptr) override {
    CAF_LOG_TRACE(CAF_ARG(ptr));
    CAF_ASSERT(ptr != nullptr);
    auto slot = ptr->slots.sender;
    return unassigned_paths_.emplace(slot, std::move(ptr)).second;
  }

  void for_each_path_impl(path_visitor& f) override {
    for (auto& kvp : paths_)
      f(*kvp.second.ptr);
  }

  bool check_paths_impl(path_algorithm algo,
                        path_predicate& pred) const noexcept override {
    auto f = [&](const typename map_type::value_type& x) {
      return pred(*x.second.ptr);
    };
    switch (algo) {
      default: // all_of
        return std::all_of(paths_.begin(), paths_.end(), f);
      case path_algorithm::any_of:
        return std::any_of(paths_.begin(), paths_.end(), f);
      case path_algorithm::none_of:
        return std::none_of(paths_.begin(), paths_.end(), f);
    }
  }

private:
  nested_managers nested_;
  downstream_manager* ptrs_[sizeof...(Ts) + 1];
  map_type paths_;
  unassigned_map_type unassigned_paths_;
};

} // namespace caf

