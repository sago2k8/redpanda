/*
 * Copyright 2021 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "cloud_storage/types.h"
#include "seastar/core/lowres_clock.hh"
#include "seastar/core/sstring.hh"
#include "seastar/util/bool_class.hh"
#include "seastarx.h"
#include "utils/named_type.h"

#include <seastar/core/io_priority_class.hh>
#include <seastar/core/scheduling.hh>

#include <filesystem>

namespace archival {

using cloud_storage::connection_limit;
using cloud_storage::local_segment_path;
using cloud_storage::remote_manifest_path;
using cloud_storage::remote_segment_path;
using cloud_storage::segment_name;

using service_metrics_disabled
  = ss::bool_class<struct service_metrics_disabled_tag>;
using per_ntp_metrics_disabled
  = ss::bool_class<struct per_ntp_metrics_disabled_tag>;

using segment_time_limit
  = named_type<ss::lowres_clock::duration, struct segment_time_limit_tag>;

/// Archiver service configuration
struct configuration {
    /// Bucket used to store all archived data
    cloud_storage_clients::bucket_name bucket_name;
    /// Initial backoff for requests to cloud storage
    ss::lowres_clock::duration cloud_storage_initial_backoff;
    /// Long upload timeout
    ss::lowres_clock::duration segment_upload_timeout;
    /// Shor upload timeout
    ss::lowres_clock::duration manifest_upload_timeout;
    /// Initial backoff for upload loop in case there is nothing to upload
    ss::lowres_clock::duration upload_loop_initial_backoff;
    /// Max backoff for upload loop in case there is nothing to upload
    ss::lowres_clock::duration upload_loop_max_backoff;
    /// Flag that indicates that service level metrics are disabled
    service_metrics_disabled svc_metrics_disabled;
    /// Flag that indicates that ntp-archiver level metrics are disabled
    per_ntp_metrics_disabled ntp_metrics_disabled;
    /// Upload time limit (if segment is not uploaded this amount of time the
    /// upload is triggered)
    std::optional<segment_time_limit> time_limit;
    /// Scheduling group that throttles archival upload
    ss::scheduling_group upload_scheduling_group{
      ss::default_scheduling_group()};
    /// I/o priority used to throttle file reads
    ss::io_priority_class upload_io_priority{ss::default_priority_class()};

    friend std::ostream& operator<<(std::ostream& o, const configuration& cfg);
};

/// \brief create scheduler service config
/// This mehtod will use shard-local redpanda configuration
/// to generate the configuration.
/// \param sg is a scheduling group used to run all uploads
/// \param p is an io priority class used to throttle upload file reads
archival::configuration get_archival_service_config(
  ss::scheduling_group sg = ss::default_scheduling_group(),
  ss::io_priority_class p = ss::default_priority_class());

} // namespace archival
