# Copyright 2022 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

# Utilities for checking the environment of a test

import os


def sample_license():
    """
    Returns the sample license from the env if it exists, asserts if its
    missing and the environment is CI
    """
    license = os.environ.get("REDPANDA_SAMPLE_LICENSE", None)
    if license is None:
        is_ci = os.environ.get("CI", "false")
        assert is_ci == "false"
        return None
    return license
