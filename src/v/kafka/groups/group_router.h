#pragma once
#include "cluster/shard_table.h"
#include "kafka/groups/coordinator_ntp_mapper.h"
#include "kafka/requests/heartbeat_request.h"
#include "kafka/requests/join_group_request.h"
#include "kafka/requests/leave_group_request.h"
#include "kafka/requests/offset_commit_request.h"
#include "kafka/requests/offset_fetch_request.h"
#include "kafka/requests/sync_group_request.h"
#include "kafka/types.h"
#include "seastarx.h"
#include "utils/concepts-enabled.h"

#include <seastar/core/reactor.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/sharded.hh>

#include <type_traits>

namespace kafka {

// clang-format off
CONCEPT(
template <typename T>
concept GroupManager =
requires(
  T m,
  join_group_request&& join_request,
  sync_group_request&& sync_request,
  heartbeat_request&& heartbeat_request,
  leave_group_request&& leave_request,
  offset_commit_request&& offset_commit_request,
  offset_fetch_request&& offset_fetch_request) {

    { m.join_group(std::move(join_request)) } ->
        ss::future<join_group_response>;

    { m.sync_group(std::move(sync_request)) } ->
        ss::future<sync_group_response>;

    { m.heartbeat(std::move(heartbeat_request)) } ->
        ss::future<heartbeat_response>;

    { m.leave_group(std::move(leave_request)) } ->
        ss::future<leave_group_response>;

    { m.offset_commit(std::move(offset_commit_request)) } ->
        ss::future<offset_commit_response>;

    { m.offset_fetch(std::move(offset_fetch_request)) } ->
        ss::future<offset_fetch_response>;
};
)
// clang-format on

/**
 * \brief Forward group operations the owning core.
 *
 * Routing an operation is a two step process. First, the coordinator key is
 * mapped to its associated ntp using the coordinator_ntp_mapper. Given the ntp
 * the owning shard is found using the cluster::shard_table. Finally, a x-core
 * operation on the destination shard's group manager is invoked.
 */
template<typename GroupMgr>
CONCEPT(requires GroupManager<GroupMgr>)
class group_router final {
public:
    group_router(
      ss::scheduling_group sched_group,
      ss::smp_service_group smp_group,
      ss::sharded<GroupMgr>& group_manager,
      ss::sharded<cluster::shard_table>& shards,
      ss::sharded<coordinator_ntp_mapper>& coordinators)
      : _sg(sched_group)
      , _ssg(smp_group)
      , _group_manager(group_manager)
      , _shards(shards)
      , _coordinators(coordinators) {}

    auto join_group(join_group_request&& request) {
        return route(std::move(request), &GroupMgr::join_group);
    }

    auto sync_group(sync_group_request&& request) {
        return route(std::move(request), &GroupMgr::sync_group);
    }

    auto heartbeat(heartbeat_request&& request) {
        return route(std::move(request), &GroupMgr::heartbeat);
    }

    auto leave_group(leave_group_request&& request) {
        return route(std::move(request), &GroupMgr::leave_group);
    }

    auto offset_commit(offset_commit_request&& request) {
        return route(std::move(request), &GroupMgr::offset_commit);
    }

    auto offset_fetch(offset_fetch_request&& request) {
        return route(std::move(request), &GroupMgr::offset_fetch);
    }

private:
    std::optional<std::pair<model::ntp, ss::shard_id>>
    shard_for(const group_id& group) {
        if (auto ntp = _coordinators.local().ntp_for(group); ntp) {
            if (auto shard_id = _shards.local().shard_for(*ntp); shard_id) {
                return std::make_pair(std::move(*ntp), *shard_id);
            }
        }
        return std::nullopt;
    }

    template<typename FwdFunc, typename Request>
    auto route(Request&& r, FwdFunc func) {
        // get response type from FwdFunc it has return future<response>.
        using return_type = std::invoke_result_t<
          FwdFunc,
          decltype(std::declval<GroupMgr>()),
          Request&&>;
        using tuple_type = typename return_type::value_type;
        static_assert(std::tuple_size_v<tuple_type> == 1);
        using resp_type = typename std::tuple_element_t<0, tuple_type>;

        auto m = shard_for(r.data.group_id);
        if (!m) {
            return ss::make_ready_future<resp_type>(
              resp_type(r, error_code::not_coordinator));
        }
        r.ntp = std::move(m->first);
        return with_scheduling_group(
          _sg, [this, func, shard = m->second, r = std::move(r)]() mutable {
              return _group_manager.invoke_on(
                shard, _ssg, [func, r = std::move(r)](GroupMgr& mgr) mutable {
                    return std::invoke(func, mgr, std::move(r));
                });
          });
    }

    ss::scheduling_group _sg;
    ss::smp_service_group _ssg;
    ss::sharded<GroupMgr>& _group_manager;
    ss::sharded<cluster::shard_table>& _shards;
    ss::sharded<coordinator_ntp_mapper>& _coordinators;
};

} // namespace kafka
