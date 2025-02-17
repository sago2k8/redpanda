// Copyright 2022 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

//go:build !linux

package bundle

import (
	"context"
	"errors"

	"github.com/spf13/afero"
)

func executeBundle(ctx context.Context, b bundleParams) error {
	return errors.New("rpk debug bundle is unsupported on your operating system")
}

func executeK8SBundle(ctx context.Context, bp bundleParams) error {
	return errors.New("rpk debug bundle is unsupported on your operating system")
}

func determineFilepath(fs afero.Fs, path string, isFlag bool) (finalPath string, err error) {
	return "", errors.New("rpk debug bundle is not supported on your operating system")
}
