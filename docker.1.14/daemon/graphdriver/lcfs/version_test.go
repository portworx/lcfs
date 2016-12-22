// +build linux

package lcfs

import (
	"testing"
)

func TestLibVersion(t *testing.T) {
	if lcfsLibVersion() <= 0 {
		t.Errorf("expected output from lcfs lib version > 0")
	}
}
