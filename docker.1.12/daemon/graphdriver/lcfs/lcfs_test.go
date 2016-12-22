// +build linux

package lcfs

import (
	"testing"

	"github.com/docker/docker/daemon/graphdriver"
	"github.com/docker/docker/daemon/graphdriver/graphtest"
	"github.com/docker/docker/pkg/archive"
)

func init() {
	// Do not sure chroot to speed run time and allow archive
	// errors or hangs to be debugged directly from the test process.
	graphdriver.ApplyUncompressedLayer = archive.ApplyUncompressedLayer
}

// Make sure lcfs file system is mounted on /tmp
//
// This avoids creating a new driver for each test if all tests are run
// Make sure to put new tests between TestLcfsSetup and TestLcfsTeardown
func TestLcfsSetup(t *testing.T) {
	graphtest.GetDriver(t, "lcfs")
}

func TestLcfsCreateEmpty(t *testing.T) {
	graphtest.DriverTestCreateEmpty(t, "lcfs")
}

func TestLcfsCreateBase(t *testing.T) {
	graphtest.DriverTestCreateBase(t, "lcfs")
}

func TestLcfsCreateSnap(t *testing.T) {
	graphtest.DriverTestCreateSnap(t, "lcfs")
}

func TestLcfs50LayerRead(t *testing.T) {
	graphtest.DriverTestDeepLayerRead(t, 50, "lcfs")
}

// Fails due to bug in calculating changes after apply
// likely related to https://github.com/docker/docker/issues/21555
func TestLcfsDiffApply10Files(t *testing.T) {
	t.Skipf("Fails to compute changes after apply intermittently")
	graphtest.DriverTestDiffApply(t, 10, "lcfs")
}

func TestLcfsChanges(t *testing.T) {
	t.Skipf("Fails to compute changes intermittently")
	graphtest.DriverTestChanges(t, "lcfs")
}

func TestLcfsTeardown(t *testing.T) {
	graphtest.PutDriver(t)
}

// Benchmarks should always setup new driver

func BenchmarkExists(b *testing.B) {
	graphtest.DriverBenchExists(b, "lcfs")
}

func BenchmarkGetEmpty(b *testing.B) {
	graphtest.DriverBenchGetEmpty(b, "lcfs")
}

func BenchmarkDiffBase(b *testing.B) {
	graphtest.DriverBenchDiffBase(b, "lcfs")
}

func BenchmarkDiffSmallUpper(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10, 10, "lcfs")
}

func BenchmarkDiff10KFileUpper(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10, 10000, "lcfs")
}

func BenchmarkDiff10KFilesBottom(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10000, 10, "lcfs")
}

func BenchmarkDiffApply100(b *testing.B) {
	graphtest.DriverBenchDiffApplyN(b, 100, "lcfs")
}

func BenchmarkDiff20Layers(b *testing.B) {
	graphtest.DriverBenchDeepLayerDiff(b, 20, "lcfs")
}

func BenchmarkRead20Layers(b *testing.B) {
	graphtest.DriverBenchDeepLayerRead(b, 20, "lcfs")
}
