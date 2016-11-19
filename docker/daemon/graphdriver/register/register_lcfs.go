// +build !exclude_graphdriver_lcfs,linux

package register

import (
	// register the lcfs graphdriver
	_ "github.com/docker/docker/daemon/graphdriver/lcfs"
)
