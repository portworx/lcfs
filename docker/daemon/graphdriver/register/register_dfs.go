// +build !exclude_graphdriver_dfs,linux

package register

import (
	// register the dfs graphdriver
	_ "github.com/docker/docker/daemon/graphdriver/dfs"
)
