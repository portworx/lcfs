// +build linux

package dfs

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

// Make sure dfs file system is mounted on /tmp
//
// This avoids creating a new driver for each test if all tests are run
// Make sure to put new tests between TestDfsSetup and TestDfsTeardown
func TestDfsSetup(t *testing.T) {
	graphtest.GetDriver(t, "dfs")
}

func TestDfsCreateEmpty(t *testing.T) {
	graphtest.DriverTestCreateEmpty(t, "dfs")
}

func TestDfsCreateBase(t *testing.T) {
	graphtest.DriverTestCreateBase(t, "dfs")
}

func TestDfsCreateSnap(t *testing.T) {
	graphtest.DriverTestCreateSnap(t, "dfs")
}

func TestDfs50LayerRead(t *testing.T) {
	graphtest.DriverTestDeepLayerRead(t, 50, "dfs")
}

// Fails due to bug in calculating changes after apply
// likely related to https://github.com/docker/docker/issues/21555
func TestDfsDiffApply10Files(t *testing.T) {
	t.Skipf("Fails to compute changes after apply intermittently")
	graphtest.DriverTestDiffApply(t, 10, "dfs")
}

func TestDfsChanges(t *testing.T) {
	t.Skipf("Fails to compute changes intermittently")
	graphtest.DriverTestChanges(t, "dfs")
}

func TestDfsTeardown(t *testing.T) {
	graphtest.PutDriver(t)
}

// Benchmarks should always setup new driver

func BenchmarkExists(b *testing.B) {
	graphtest.DriverBenchExists(b, "dfs")
}

func BenchmarkGetEmpty(b *testing.B) {
	graphtest.DriverBenchGetEmpty(b, "dfs")
}

func BenchmarkDiffBase(b *testing.B) {
	graphtest.DriverBenchDiffBase(b, "dfs")
}

func BenchmarkDiffSmallUpper(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10, 10, "dfs")
}

func BenchmarkDiff10KFileUpper(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10, 10000, "dfs")
}

func BenchmarkDiff10KFilesBottom(b *testing.B) {
	graphtest.DriverBenchDiffN(b, 10000, 10, "dfs")
}

func BenchmarkDiffApply100(b *testing.B) {
	graphtest.DriverBenchDiffApplyN(b, 100, "dfs")
}

func BenchmarkDiff20Layers(b *testing.B) {
	graphtest.DriverBenchDeepLayerDiff(b, 20, "dfs")
}

func BenchmarkRead20Layers(b *testing.B) {
	graphtest.DriverBenchDeepLayerRead(b, 20, "dfs")
}
