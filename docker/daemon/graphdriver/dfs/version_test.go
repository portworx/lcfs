// +build linux

package dfs

import (
	"testing"
)

func TestLibVersion(t *testing.T) {
	if dfsLibVersion() <= 0 {
		t.Errorf("expected output from dfs lib version > 0")
	}
}
