/*
 * Copyright 2022 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#include "cloud_storage/topic_recovery_service.h"

#include "cloud_storage/logger.h"
#include "cloud_storage/recovery_utils.h"
#include "cloud_storage/topic_manifest.h"
#include "cluster/topic_recovery_status_frontend.h"
#include "cluster/topics_frontend.h"

#include <seastar/util/defer.hh>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/outcome/try.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace {

const std::regex manifest_path_expr{
  R"REGEX(\w+/meta/(.*?)/(.*?)/topic_manifest.json)REGEX"};

constexpr size_t list_api_timeout_multiplier{10};

constexpr ss::lowres_clock::duration downloads_check_interval{60s};

ss::lowres_clock::duration load_downloads_check_interval() {
    auto tick = std::getenv("__REDPANDA_TOPIC_REC_DL_CHECK_MILLIS");
    if (tick) {
        try {
            return ss::lowres_clock::duration{std::stoll(tick)};
        } catch (const std::invalid_argument&) {
            vlog(
              cloud_storage::cst_log.warn,
              "The environment variable "
              "__REDPANDA_TOPIC_REC_DL_CHECK_MILLIS is set to {} but "
              "could not be converted to a duration.",
              tick);
            return downloads_check_interval;
        }
    } else {
        return downloads_check_interval;
    }
}

retry_chain_node
make_rtc(ss::abort_source& as, const cloud_storage::recovery_task_config& cfg) {
    return retry_chain_node{as, cfg.operation_timeout_ms, cfg.backoff_ms};
}

} // namespace

namespace cloud_storage {

std::ostream& operator<<(std::ostream& os, const init_recovery_result& result) {
    fmt::print(
      os,
      "{{status_code: {}, message: {}}}",
      static_cast<int>(result.status_code),
      result.message);
    return os;
}

std::ostream& operator<<(std::ostream& os, const topic_download_status& tds) {
    fmt::print(
      os,
      "{{pending_downloads: {}, successful_downloads: {}, failed_downloads: "
      "{}}}",
      tds.pending_downloads,
      tds.successful_downloads,
      tds.failed_downloads);
    return os;
}

static ss::sstring get_value_or_throw(
  const config::property<std::optional<ss::sstring>>& prop, const char* name) {
    auto opt = prop.value();
    if (!opt) {
        vlog(
          cst_log.error,
          "Configuration property {} is required to enable archival storage",
          name);
        throw std::runtime_error(
          fmt::format("configuration property {} is not set", name));
    }
    return *opt;
}

recovery_task_config recovery_task_config::make_config() {
    return recovery_task_config{
      .bucket = cloud_storage_clients::bucket_name(get_value_or_throw(
        cloud_storage::configuration::get_bucket_config(),
        "cloud_storage_bucket")),
      .operation_timeout_ms
      = config::shard_local_cfg()
          .cloud_storage_manifest_upload_timeout_ms.value(),
      .backoff_ms = config::shard_local_cfg()
                      .cloud_storage_upload_loop_initial_backoff_ms.value()};
}

topic_recovery_service::topic_recovery_service(
  ss::sharded<remote>& remote,
  ss::sharded<cluster::topic_table>& topic_state,
  ss::sharded<cluster::topics_frontend>& topics_frontend,
  ss::sharded<cluster::topic_recovery_status_frontend>&
    topic_recovery_status_frontend)
  : _config(recovery_task_config::make_config())
  , _remote{remote}
  , _topic_state{topic_state}
  , _topics_frontend{topics_frontend}
  , _topic_recovery_status_frontend{topic_recovery_status_frontend} {}

ss::future<> topic_recovery_service::stop() {
    co_await _gate.close();
    vlog(cst_log.debug, "stopped topic recovery service");
}

ss::future<> topic_recovery_service::shutdown_recovery() {
    vlog(cst_log.debug, "shutting down recovery if active");
    if (_pending_status_timer.cancel()) {
        vlog(cst_log.warn, "cancelled active pending status timer");
    }
    _as.request_abort();
    co_return;
}

init_recovery_result
topic_recovery_service::start_recovery(ss::httpd::request req) {
    try {
        if (is_active()) {
            vlog(cst_log.warn, "A recovery is already active");
            return {
              .status_code = ss::httpd::reply::status_type::conflict,
              .message = "A recovery is already active"};
        }

        recovery_request request{std::move(req)};
        ssx::spawn_with_gate(_gate, [this, r = std::move(request)]() mutable {
            return start_bg_recovery_task(std::move(r)).then([](auto result) {
                if (result.has_error()) {
                    vlog(
                      cst_log.error,
                      "topic recovery completed with error: {}",
                      result.error().context);
                } else {
                    vlog(
                      cst_log.info,
                      "topic recovery completed. Pending downloads");
                }
            });
        });
        return {
          .status_code = ss::httpd::reply::status_type::accepted,
          .message = "recovery started"};
    } catch (const bad_request& ex) {
        return {
          .status_code = ss::httpd::reply::status_type::bad_request,
          .message = fmt::format(
            "bad recovery request payload: {}", ex.what())};
    } catch (const std::exception& ex) {
        return {
          .status_code = ss::httpd::reply::status_type::internal_server_error,
          .message = fmt::format(
            "recovery init failed with error: {}", ex.what())};
    }
}

ss::future<> topic_recovery_service::propagate_state(state s) {
    return container().invoke_on_all([s](auto& svc) {
        svc._state = s;
        return ss::make_ready_future<>();
    });
}

ss::future<result<void, recovery_error_ctx>>
topic_recovery_service::start_bg_recovery_task(recovery_request request) {
    if (is_active()) {
        vlog(cst_log.warn, "A recovery is already active");
        co_return recovery_error_ctx::make(
          "A recovery is already active",
          recovery_error_code::recovery_already_running);
    }

    vlog(cst_log.info, "Starting recovery task with request: {}", request);
    // Setting this state here ensures that another request coming in soon after
    // on the same shard is rejected. If we set this state after the RPC call,
    // it is possible that two requests sent back to back on the controller
    // leader assume that no other recovery is running.
    _state = state::starting;
    if (_pending_status_timer.cancel()) {
        vlog(
          cst_log.warn,
          "stopped running pending status timer before starting recovery "
          "request");
    }

    vlog(
      cst_log.debug, "checking if recovery is already running on another node");

    // When making RPC call we make sure that we do not check the state of the
    // current shard, because recovery is in starting state here.
    if (co_await _topic_recovery_status_frontend.local().is_recovery_running(
          container(),
          cluster::topic_recovery_status_frontend::skip_this_node::yes)) {
        vlog(cst_log.warn, "A recovery is already active");
        _state = state::inactive;
        co_return recovery_error_ctx::make(
          "A recovery is already active",
          recovery_error_code::recovery_already_running);
    }

    auto fib = make_rtc(_as, _config);

    co_await propagate_state(state::scanning_bucket);
    vlog(cst_log.debug, "scanning bucket {}", _config.bucket);
    auto bucket_contents_result = co_await _remote.local().list_objects(
      _config.bucket, fib);

    if (bucket_contents_result.has_error()) {
        auto error = recovery_error_ctx::make(
          fmt::format(
            "error while listing items: {}", bucket_contents_result.error()),
          recovery_error_code::error_listing_items);
        vlog(cst_log.error, "{}", error.context);
        co_await propagate_state(state::inactive);
        co_return error;
    }

    auto bucket_contents = bucket_contents_result.value();

    auto manifests = co_await filter_existing_topics(
      bucket_contents, request, model::ns{"kafka"});

    if (manifests.empty()) {
        vlog(cst_log.info, "exiting recovery, no topics to create");
        co_await propagate_state(state::inactive);
        co_return outcome::success();
    }

    vlog(cst_log.info, "found {} topics to create", manifests.size());
    for (const auto& manifest : manifests) {
        vlog(cst_log.debug, "topic manifest: {}", manifest.get_manifest_path());
    }
    _recovery_status.clear();

    auto clear_fib = make_rtc(_as, _config);
    co_await clear_recovery_results(
      _remote.local(), _config.bucket, clear_fib, std::nullopt);
    _downloaded_manifests.emplace(manifests);

    populate_recovery_status();

    co_await propagate_state(state::creating_topics);
    vlog(cst_log.debug, "creating topics");

    result<void, recovery_error_ctx> result = outcome::success();
    try {
        auto create_topics_results = co_await create_topics(request);
        for (const auto& r : create_topics_results) {
            if (r.ec != cluster::errc::success) {
                vlog(cst_log.warn, "topic creation failed: {}", r);
                _recovery_status.erase(r.tp_ns);
            } else {
                vlog(
                  cst_log.debug,
                  "topic created: {}, expected downloads: {}",
                  r,
                  _recovery_status[r.tp_ns].pending_downloads);
            }
        }

        co_await propagate_state(state::recovering_data);
        start_download_bg_tracker();
    } catch (const ss::timed_out_error& err) {
        result = recovery_error_ctx::make(
          fmt::format("failed to create topics: {}", err),
          recovery_error_code::error_creating_topics);
    }

    if (result.has_error()) {
        co_await propagate_state(state::inactive);
    }
    co_return result;
}

static cluster::topic_configuration make_topic_config(
  const cloud_storage::topic_manifest& tm, const recovery_request& request) {
    auto topic_config = tm.get_topic_config();
    if (!topic_config.has_value()) {
        vlog(
          cst_log.warn,
          "skipping topic creation for {}, missing values in manifest",
          tm.get_manifest_path());
    }

    cluster::topic_configuration topic_to_create_cfg(
      topic_config->tp_ns.ns,
      topic_config->tp_ns.tp,
      topic_config->partition_count,
      topic_config->replication_factor);

    auto& topic_properties = topic_to_create_cfg.properties;
    auto manifest_props = topic_config->properties;

    topic_properties.compression = manifest_props.compression;
    topic_properties.cleanup_policy_bitflags
      = manifest_props.cleanup_policy_bitflags;
    topic_properties.compaction_strategy = manifest_props.compaction_strategy;

    topic_properties.retention_bytes = manifest_props.retention_bytes;
    topic_properties.retention_duration = manifest_props.retention_duration;

    topic_properties.retention_local_target_bytes = tristate<size_t>{
      config::shard_local_cfg()
        .cloud_storage_recovery_temporary_retention_bytes_default};

    topic_properties.segment_size = manifest_props.segment_size;
    topic_properties.timestamp_type = manifest_props.timestamp_type;
    topic_properties.shadow_indexing = model::shadow_indexing_mode::full;
    topic_properties.recovery = true;

    if (request.retention_bytes().has_value()) {
        topic_properties.retention_local_target_bytes = tristate<size_t>{
          request.retention_bytes()};
        topic_properties.retention_local_target_ms = {};
    } else if (request.retention_ms().has_value()) {
        topic_properties.retention_local_target_ms
          = tristate<std::chrono::milliseconds>{request.retention_ms()};
        topic_properties.retention_local_target_bytes = {};
    }

    return topic_to_create_cfg;
}

ss::future<std::vector<cluster::topic_result>>
topic_recovery_service::create_topics(const recovery_request& request) {
    std::vector<cluster::topic_configuration> topic_configs;
    topic_configs.reserve(_downloaded_manifests->size());

    std::transform(
      _downloaded_manifests->cbegin(),
      _downloaded_manifests->cend(),
      std::back_inserter(topic_configs),
      [&request](const auto& m) { return make_topic_config(m, request); });

    co_return co_await _topics_frontend.local().autocreate_topics(
      topic_configs, config::shard_local_cfg().create_topic_timeout_ms());
}

ss::future<std::vector<cloud_storage::topic_manifest>>
topic_recovery_service::filter_existing_topics(
  const remote::list_bucket_items& items,
  const recovery_request& request,
  std::optional<model::ns> filter_ns) {
    absl::flat_hash_map<ss::sstring, absl::flat_hash_set<ss::sstring>>
      topic_index;

    for (const auto& topic : _topic_state.local().all_topics()) {
        topic_index.try_emplace(topic.ns, absl::flat_hash_set<ss::sstring>{});
        topic_index[topic.ns].insert(topic.tp);
    }

    std::vector<topic_manifest> manifests;
    manifests.reserve(items.size());

    std::optional<std::regex> requested_pattern = std::nullopt;
    if (request.topic_names_pattern().has_value()) {
        requested_pattern.emplace(
          request.topic_names_pattern().value().data(),
          request.topic_names_pattern().value().size());
    }

    for (const auto& item : items) {
        std::string s{item.key.data(), item.key.size()};

        std::smatch matches;
        const auto is_topic_manifest = std::regex_match(
          s, matches, manifest_path_expr);
        if (!is_topic_manifest) {
            continue;
        }

        const auto& ns = matches[1].str();
        const auto& tp = matches[2].str();

        if (
          requested_pattern.has_value()
          && !std::regex_search(tp, requested_pattern.value())) {
            vlog(
              cst_log.debug,
              "will skip topic {}, it does not match pattern {}",
              tp,
              request.topic_names_pattern().value());
            continue;
        }

        if (topic_index.contains(ns) && topic_index[ns].contains(tp)) {
            vlog(
              cst_log.debug,
              "will skip creating {}:{}, topic already exists",
              ns,
              tp);
            continue;
        }

        if (auto download_r = co_await download_manifest(item.key);
            download_r.has_value()) {
            manifests.push_back(std::move(download_r.value()));
        }
    }
    co_return manifests;
}

ss::future<result<cloud_storage::topic_manifest, recovery_error_ctx>>
topic_recovery_service::download_manifest(ss::sstring path) {
    cloud_storage::topic_manifest m;
    auto fib = make_rtc(_as, _config);
    try {
        auto download_r = co_await _remote.local().download_manifest(
          _config.bucket, remote_manifest_path{path}, m, fib);
        if (download_r != download_result::success) {
            auto error = recovery_error_ctx::make(
              fmt::format(
                "failed to download manifest from {}: {}", path, download_r),
              recovery_error_code::error_downloading_manifest);
            vlog(cst_log.error, "{}", error.context);
            co_return error;
        }
        co_return m;

    } catch (const std::exception& ex) {
        auto error = recovery_error_ctx::make(
          fmt::format(
            "failed to download manifest from {}: {}", path, ex.what()),
          recovery_error_code::error_downloading_manifest);
        vlog(cst_log.error, "{}", error.context);
        co_return error;
    }
}

void topic_recovery_service::start_download_bg_tracker() {
    _pending_status_timer.set_callback([this] {
        ssx::spawn_with_gate(_gate, [this] { return check_for_downloads(); });
    });

    _pending_status_timer.arm(
      ss::lowres_clock::now() + load_downloads_check_interval());
    vlog(cst_log.debug, "waiting for downloads to finish");
}

ss::future<> topic_recovery_service::reset_topic_configurations() {
    if (!_downloaded_manifests.has_value()) {
        vlog(cst_log.warn, "No manifests to reset config from");
        co_return;
    }

    std::vector<cluster::topic_properties_update> updates;
    updates.reserve(_downloaded_manifests->size());
    std::transform(
      std::make_move_iterator(_downloaded_manifests->begin()),
      std::make_move_iterator(_downloaded_manifests->end()),
      std::back_inserter(updates),
      [](auto&& tm) {
          auto update = cluster::topic_properties_update{
            tm.get_topic_config()->tp_ns};

          update.properties.retention_local_target_ms.op
            = cluster::incremental_update_operation::set;
          update.properties.retention_local_target_ms.value
            = tristate<std::chrono::milliseconds>{
              config::shard_local_cfg().retention_local_target_ms_default()};

          update.properties.retention_local_target_bytes.op
            = cluster::incremental_update_operation::set;
          update.properties.retention_local_target_bytes.value
            = tristate<size_t>{
              config::shard_local_cfg().retention_local_target_bytes_default()};

          vlog(
            cst_log.debug,
            "resetting topic properties for {} using update: {}",
            tm.get_topic_config()->tp_ns,
            update);
          return update;
      });
    auto update_results
      = co_await _topics_frontend.local().update_topic_properties(
        std::move(updates), model::no_timeout);

    for (const auto& result : update_results) {
        vlog(
          cst_log.info,
          "topic update result for {}: {}",
          result.tp_ns,
          result.ec);
    }

    _downloaded_manifests.reset();
}

ss::future<> topic_recovery_service::do_check_for_downloads() {
    vlog(cst_log.trace, "checking for downloads");
    auto fib = retry_chain_node{
      _as,
      _config.operation_timeout_ms * list_api_timeout_multiplier,
      _config.backoff_ms};
    auto results = co_await gather_recovery_results(
      _remote.local(), _config.bucket, fib);
    for (const auto& result : results) {
        if (!_recovery_status.contains(result.tp_ns)) {
            vlog(cst_log.debug, "unexpected status file: {}", result.tp_ns);
            continue;
        }

        auto& status = _recovery_status[result.tp_ns];
        status.pending_downloads -= 1;

        if (result.result) {
            status.successful_downloads += 1;
        } else {
            status.failed_downloads += 1;
        }

        vlog(
          cst_log.debug,
          "processing result for {}: [{}]. current status: {}",
          result.tp_ns,
          result.result,
          status);
    }

    auto clear_fib = make_rtc(_as, _config);
    co_await clear_recovery_results(
      _remote.local(), _config.bucket, clear_fib, std::move(results));

    for (const auto& [tp_ns, status] : _recovery_status) {
        // Try again if any ntp has pending downloads
        if (status.pending_downloads > 0) {
            _pending_status_timer.arm(
              ss::lowres_clock::now() + load_downloads_check_interval());
            co_return;
        } else {
            vlog(cst_log.trace, "all downloads complete for {}", tp_ns);
        }
    }

    // Finished with all downloads
    _recovery_status.clear();

    co_await reset_topic_configurations();
    co_await propagate_state(state::inactive);
}

ss::future<> topic_recovery_service::check_for_downloads() {
    // We need a lock here because if the check for downloads runs in parallel,
    // which is possible when there are many result files to process and one
    // call is still running while, the status hashmap can get corrupted due to
    // double processing of result files.
    return ss::with_semaphore(
      _download_check_sem, 1, [this] { return do_check_for_downloads(); });
}

void topic_recovery_service::populate_recovery_status() {
    if (!_downloaded_manifests.has_value()) {
        vlog(cst_log.warn, "No manifest to populate status from.");
        return;
    }
    for (const auto& m : _downloaded_manifests.value()) {
        auto ntp_cfg = m.get_topic_config();
        if (!ntp_cfg) {
            vlog(
              cst_log.warn,
              "skipping {}, missing ntp config in manifest",
              m.get_manifest_path());
            continue;
        }
        auto topic = ntp_cfg->tp_ns.tp;
        auto expected = ntp_cfg->partition_count * ntp_cfg->replication_factor;
        _recovery_status.emplace(
          ntp_cfg->tp_ns, topic_download_status{expected, 0, 0});
    }
}

} // namespace cloud_storage
